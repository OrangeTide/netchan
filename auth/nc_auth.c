/* nc_auth.c : SSH-shaped client authentication over an established session */

#include "nc_auth_int.h"
#include "monocypher.h"

#include <string.h>

#define SIG_LABEL     "netchan-auth-v1"

void
nc_auth_i_emit(struct nc_auth *a, const uint8_t *msg, size_t len)
{
    if (a->send)
        a->send(a->send_ctx, msg, len);
}

void
nc_auth_signed_digest(uint8_t out[32], const uint8_t sid[32],
                      const char *user, const uint8_t pk[32])
{
    crypto_blake2b_ctx ctx;
    size_t ulen = strlen(user);
    uint8_t n = (uint8_t)(ulen > NC_AUTH_MAX_USER ? NC_AUTH_MAX_USER : ulen);

    crypto_blake2b_init(&ctx, 32);
    crypto_blake2b_update(&ctx, (const uint8_t *)SIG_LABEL, sizeof(SIG_LABEL));
    crypto_blake2b_update(&ctx, sid, 32);
    crypto_blake2b_update(&ctx, &n, 1);
    crypto_blake2b_update(&ctx, (const uint8_t *)user, n);
    crypto_blake2b_update(&ctx, pk, 32);
    crypto_blake2b_final(&ctx, out);
}

void
nc_auth_client_init(struct nc_auth *a, const uint8_t sid[32], const char *user,
                    nc_auth_send_cb send, void *send_ctx)
{
    memset(a, 0, sizeof(*a));
    a->server = 0;
    a->state = NC_AUTH_STATE_PENDING;
    a->need = NC_AUTH_NEED_NOTHING;
    memcpy(a->sid, sid, 32);
    if (user) {
        strncpy(a->user, user, NC_AUTH_MAX_USER);
        a->user[NC_AUTH_MAX_USER] = '\0';
    }
    a->send = send;
    a->send_ctx = send_ctx;
}

void
nc_auth_server_init(struct nc_auth *a, const uint8_t sid[32],
                    const struct nc_auth_server_cb *cb,
                    nc_auth_send_cb send, void *send_ctx)
{
    memset(a, 0, sizeof(*a));
    a->server = 1;
    a->state = NC_AUTH_STATE_PENDING;
    memcpy(a->sid, sid, 32);
    if (cb)
        a->scb = *cb;
    a->send = send;
    a->send_ctx = send_ctx;
}

int
nc_auth_state(const struct nc_auth *a)
{
    return a->state;
}

const char *
nc_auth_user(const struct nc_auth *a)
{
    return a->user;
}

void
nc_auth_start(struct nc_auth *a)
{
    uint8_t msg[2 + NC_AUTH_MAX_USER];
    size_t ulen;

    if (a->server || a->state != NC_AUTH_STATE_PENDING)
        return;

    ulen = strlen(a->user);
    msg[0] = MSG_HELLO;
    msg[1] = (uint8_t)ulen;
    memcpy(msg + 2, a->user, ulen);
    nc_auth_i_emit(a, msg, 2 + ulen);
}

/****************************************************************
 * Client side
 ****************************************************************/

/*
 * Pick the best method still on the table and ask the application for what it
 * needs. Public key first, because a client that has one can answer without
 * troubling the user; password only when the key was refused or absent.
 *
 * Nothing is sent here. Each method is dropped from avail as it is chosen, so
 * a failure walks down the list instead of retrying the same thing forever,
 * and the conversation parks until a supply call arrives.
 *
 * One question outranks that order, because the machine cannot answer it: a
 * server offering to make an account is asking something only the player
 * knows. Offered publickey and password alone, nothing suspends and this
 * behaves exactly as it did before the interactive method existed.
 */
void
nc_auth_i_client_try_next(struct nc_auth *a)
{
    if (a->state != NC_AUTH_STATE_PENDING)
        return;

    if (!a->asked &&
        (a->avail & (NC_AUTH_M_REGISTER | NC_AUTH_M_INTERACTIVE))) {
        if (a->chose) {
            nc_auth_i_ia_choose(a);
            return;
        }
        a->need = NC_AUTH_NEED_METHOD;
        return;
    }

    if (a->avail & NC_AUTH_M_PUBKEY) {
        a->avail &= ~(unsigned)NC_AUTH_M_PUBKEY;
        a->need = NC_AUTH_NEED_KEY;
        return;
    }
    if (a->avail & NC_AUTH_M_PASSWORD) {
        a->avail &= ~(unsigned)NC_AUTH_M_PASSWORD;
        a->need = NC_AUTH_NEED_PASSWORD;
        return;
    }

    a->need = NC_AUTH_NEED_NOTHING;
    a->state = NC_AUTH_STATE_DENIED;
}

int
nc_auth_needs(const struct nc_auth *a)
{
    return a->need;
}

void
nc_auth_supply_key(struct nc_auth *a, const uint8_t sk[64], const uint8_t pk[32])
{
    uint8_t msg[1 + 32 + 64];
    uint8_t digest[32];

    if (a->server || a->need != NC_AUTH_NEED_KEY)
        return;
    a->need = NC_AUTH_NEED_NOTHING;

    if (!sk || !pk) {
        nc_auth_i_client_try_next(a);         /* no key enrolled: fall through */
        return;
    }

    nc_auth_signed_digest(digest, a->sid, a->user, pk);
    msg[0] = MSG_PUBKEY;
    memcpy(msg + 1, pk, 32);
    crypto_eddsa_sign(msg + 33, sk, digest, sizeof(digest));
    nc_auth_i_emit(a, msg, sizeof(msg));
    crypto_wipe(msg, sizeof(msg));
}

void
nc_auth_supply_password(struct nc_auth *a, const char *password)
{
    uint8_t msg[2 + NC_AUTH_MAX_PASS];
    size_t plen;

    if (a->server || a->need != NC_AUTH_NEED_PASSWORD)
        return;
    a->need = NC_AUTH_NEED_NOTHING;

    if (!password) {
        nc_auth_i_client_try_next(a);         /* the user gave up */
        return;
    }

    plen = strlen(password);
    if (plen > NC_AUTH_MAX_PASS - 1) {
        nc_auth_i_client_try_next(a);
        return;
    }

    msg[0] = MSG_PASSWORD;
    msg[1] = (uint8_t)plen;
    memcpy(msg + 2, password, plen);
    nc_auth_i_emit(a, msg, 2 + plen);
    crypto_wipe(msg, sizeof(msg));
}

static int
client_feed(struct nc_auth *a, const uint8_t *msg, size_t len)
{
    switch (msg[0]) {
    case MSG_METHODS:
        if (len != 2)
            return NC_AUTH_ERR;
        /* A fresh offer also ends any interactive method that was running.
         * Registration lands here when it finishes and when it is cancelled,
         * and the difference is whether we were the ones who gave up. */
        if (a->ia_method == NC_AUTH_M_REGISTER && !a->cancelling)
            a->registered = 1;
        a->ia_method = 0;
        a->cancelling = 0;
        a->asked = 0;
        a->wait_note = NULL;
        a->avail = msg[1];
        a->offered = msg[1];
        if (a->avail == 0) {
            a->state = NC_AUTH_STATE_DENIED;
            return NC_AUTH_OK;
        }
        nc_auth_i_client_try_next(a);
        return NC_AUTH_OK;

    case MSG_OK:
        a->state = NC_AUTH_STATE_OK;
        return NC_AUTH_OK;

    case MSG_FAIL:
        if (len != 2)
            return NC_AUTH_ERR;
        /* The server tells us what is still worth trying, so we never guess
         * and never retry a method it has already ruled out. */
        a->avail &= msg[1];
        a->ia_method = 0;
        a->cancelling = 0;
        /* The situation changed, so a player who chose to log in may now want
         * the account they turned out not to have. Ask again if it is still
         * on the table. */
        a->asked = 0;
        nc_auth_i_client_try_next(a);
        return NC_AUTH_OK;

    default:
        return nc_auth_i_ia_client(a, msg, len);
    }
}

/****************************************************************
 * Server side
 ****************************************************************/

static void
server_reply(struct nc_auth *a, uint8_t type, unsigned mask, int has_arg)
{
    uint8_t msg[2];

    msg[0] = type;
    msg[1] = (uint8_t)mask;
    nc_auth_i_emit(a, msg, has_arg ? 2 : 1);
}

void
nc_auth_i_server_deny(struct nc_auth *a, unsigned spent)
{
    a->avail &= ~spent;
    if (++a->tries >= NC_AUTH_MAX_TRIES || a->avail == 0) {
        a->avail = 0;
        a->state = NC_AUTH_STATE_DENIED;
    }
    server_reply(a, MSG_FAIL, a->avail, 1);
}

/*
 * Offer the methods again. A registration that finished, and one that was
 * abandoned, both land here: the conversation returns to where it was before
 * the method began, and the callback is asked afresh because by then the
 * answer has changed, the user now exists.
 */
void
nc_auth_i_server_offer(struct nc_auth *a)
{
    a->avail = a->scb.methods ? a->scb.methods(a->scb.ctx, a->user) : 0;
    if (a->avail == 0)
        a->state = NC_AUTH_STATE_DENIED;
    server_reply(a, MSG_METHODS, a->avail, 1);
}

static int
server_feed(struct nc_auth *a, const uint8_t *msg, size_t len)
{
    switch (msg[0]) {
    case MSG_HELLO: {
        size_t ulen;

        if (a->greeted || len < 2)
            return NC_AUTH_ERR;
        ulen = msg[1];
        if (ulen > NC_AUTH_MAX_USER || len != 2 + ulen)
            return NC_AUTH_ERR;
        memcpy(a->user, msg + 2, ulen);
        a->user[ulen] = '\0';
        a->greeted = 1;
        a->avail = a->scb.methods ? a->scb.methods(a->scb.ctx, a->user) : 0;
        if (a->avail == 0)
            a->state = NC_AUTH_STATE_DENIED;
        server_reply(a, MSG_METHODS, a->avail, 1);
        return NC_AUTH_OK;
    }

    case MSG_PUBKEY: {
        uint8_t digest[32];
        const uint8_t *pk = msg + 1;
        const uint8_t *sig = msg + 33;

        if (!a->greeted || len != 1 + 32 + 64)
            return NC_AUTH_ERR;
        if (!(a->avail & NC_AUTH_M_PUBKEY)) {
            nc_auth_i_server_deny(a, 0);
            return NC_AUTH_OK;
        }
        /* Possession of the secret key first, authorisation second. The
         * digest is bound to this session, so a signature lifted from
         * another one does not check out here. */
        nc_auth_signed_digest(digest, a->sid, a->user, pk);
        if (crypto_eddsa_check(sig, pk, digest, sizeof(digest)) == 0 &&
            a->scb.check_key && a->scb.check_key(a->scb.ctx, a->user, pk)) {
            a->state = NC_AUTH_STATE_OK;
            server_reply(a, MSG_OK, 0, 0);
            return NC_AUTH_OK;
        }
        nc_auth_i_server_deny(a, NC_AUTH_M_PUBKEY);
        return NC_AUTH_OK;
    }

    case MSG_PASSWORD: {
        char pass[NC_AUTH_MAX_PASS];
        size_t plen;
        int ok;

        if (!a->greeted || len < 2)
            return NC_AUTH_ERR;
        plen = msg[1];
        if (plen > NC_AUTH_MAX_PASS - 1 || len != 2 + plen)
            return NC_AUTH_ERR;
        if (!(a->avail & NC_AUTH_M_PASSWORD)) {
            nc_auth_i_server_deny(a, 0);
            return NC_AUTH_OK;
        }
        memcpy(pass, msg + 2, plen);
        pass[plen] = '\0';
        ok = a->scb.check_password &&
             a->scb.check_password(a->scb.ctx, a->user, pass);
        crypto_wipe(pass, sizeof(pass));
        if (ok) {
            a->state = NC_AUTH_STATE_OK;
            server_reply(a, MSG_OK, 0, 0);
            return NC_AUTH_OK;
        }
        nc_auth_i_server_deny(a, NC_AUTH_M_PASSWORD);
        return NC_AUTH_OK;
    }

    default:
        return nc_auth_i_ia_server(a, msg, len);
    }
}

int
nc_auth_feed(struct nc_auth *a, const void *msg, size_t len)
{
    const uint8_t *m = msg;
    size_t cap;
    int rc;

    if (len < 1 || a->state != NC_AUTH_STATE_PENDING)
        return NC_AUTH_ERR;
    /* A form and its answers are the only messages that outgrow a datagram,
     * and they are bounded instead by what the caller's buffer will hold. */
    cap = MSG_IS_BULK(m[0]) ? NC_FORM_MAX_MSG : NC_AUTH_MAX_MSG;
    if (len > cap)
        return NC_AUTH_ERR;

    rc = a->server ? server_feed(a, m, len) : client_feed(a, m, len);
    if (rc != 0)
        a->state = NC_AUTH_STATE_DENIED;
    return rc;
}
