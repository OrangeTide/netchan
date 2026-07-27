/* auth_link.h : authenticated encrypted netchan session on the iox loop */

#ifndef AUTH_LINK_H
#define AUTH_LINK_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "nc_addr.h"
#include "nc_crypto.h"
#include "nc_auth.h"

#define AUTH_LINK_OK  (0)
#define AUTH_LINK_ERR (-1)

struct iox_loop;

/*
 * Four layers, stacked, with the event loop underneath all of them:
 *
 *   nc_auth      who the client is          (messages on a reliable channel)
 *   netchan      reliable ordered delivery  (the protocol core)
 *   nc_crypto    secrecy and server identity (transport decorator)
 *   iox          socket readiness, timers, signals
 *
 * Each one is ignorant of the others. netchan does not know it is encrypted;
 * nc_crypto does not know a login is happening above it; nc_auth does not
 * know what carries its messages. auth_link is the only file that has to
 * hold all four in view, and it is small because the seams are real.
 *
 * The ordering is fixed and it matters. The crypto handshake completes
 * first, so netchan's own SYN travels sealed. netchan connects, which gives
 * a reliable channel. Only then does the login run, over that channel, with
 * its signature bound to the crypto session id. Application bytes flow last,
 * and never before the login succeeds.
 *
 * A session here is always encrypted. Authentication without secrecy would
 * put a password on the wire in the clear and hand an eavesdropper the
 * signature to replay, so the two are not separable options.
 */

struct auth_link;

enum {
    AL_DOWN_PEER = 0,       /* the peer disconnected */
    AL_DOWN_AUTH = 1,       /* the login was refused */
    AL_DOWN_HOSTKEY = 2,    /* the server's identity key was not accepted */
};

typedef void (*al_up_cb)(struct auth_link *al, void *user);
typedef void (*al_data_cb)(struct auth_link *al,
                           const uint8_t *data, size_t len, void *user);
typedef void (*al_down_cb)(struct auth_link *al, int reason, void *user);

/*
 * The login needs something from the human: a key, a password, a choice
 * between logging in and making an account, or a filled-in form. Nothing is
 * waiting on the answer, so the handler is free to return immediately and
 * supply it later, which is how a client prompts a human without stalling the
 * loop the session depends on.
 *
 * what is one of the NC_AUTH_NEED_* values, so a client that only ever does
 * publickey can ignore the two new ones and behave as it always did.
 */
typedef void (*al_need_cb)(struct auth_link *al, int what, void *user);

struct auth_link_cb {
    al_up_cb   on_up;       /* fires only after authentication succeeds */
    al_data_cb on_data;
    al_down_cb on_down;
    al_need_cb on_need;     /* client only */
    void      *user;
};

struct auth_link_cfg {
    int server;                          /* 1 accepting, 0 connecting */
    const struct nc_addr *peer;          /* client: the server address */
    const uint8_t *static_sk;            /* 32-byte identity secret, or NULL */
    const uint8_t *psk;                  /* 32-byte pre-shared key, or NULL */
    int require_peer_static;             /* refuse an anonymous peer */
    nc_crypto_verify_cb verify_peer;     /* client: the known-hosts decision */
    void *verify_ctx;
    const char *user;                    /* client: the name to log in as */
    struct nc_auth_server_cb scb;        /* server: the credential store */
    struct nc_auth_ia_cb iacb;           /* server: registration, optional */
};

/* Create a session over an already-bound, non-blocking UDP socket fd. The
 * link registers its own fd watcher and service timer on the loop. */
struct auth_link *auth_link_open(struct iox_loop *loop, int fd,
                                 const struct auth_link_cfg *cfg,
                                 const struct auth_link_cb *cb);

/*
 * Answer an on_need callback, whenever the answer happens to arrive. Pass
 * NULL to say the credential is not available, which drops that method and
 * lets the login try the next one.
 */
void auth_link_supply_key(struct auth_link *al, const uint8_t *sk,
                          const uint8_t *pk);
void auth_link_supply_password(struct auth_link *al, const char *password);

/****************************************************************
 * Registering, for a client that has no account yet
 *
 * A public server meets players it has never seen, so it offers a method that
 * makes an account rather than checking one. The client answers
 * NC_AUTH_NEED_METHOD with NC_AUTH_M_REGISTER, fills in whatever form the
 * server sends, and lands back at the method offer with an account to log in
 * with. See docs/content/registration/ for why it works that way.
 *
 * None of this needs a second connection, a second channel, or a second
 * credential store. It is the same conversation on the same channel.
 ****************************************************************/

/* Answer NC_AUTH_NEED_METHOD. Passing 0 abandons the login. */
void auth_link_supply_method(struct auth_link *al, unsigned method);

/* The form to draw, valid until the next answer is supplied. NULL unless the
 * link is sitting on NC_AUTH_NEED_FORM. */
const struct nc_form *auth_link_form(const struct auth_link *al);

/* Answer the form. vals is parallel to the form's fields. */
void auth_link_submit(struct auth_link *al, const struct nc_form_value *vals,
                      int n);

/* Give up on the form, or on waiting for an emailed code, and go back to the
 * method offer with the account one already had. */
void auth_link_cancel(struct auth_link *al);

/* What the server said it is waiting on, or NULL. Set between "I gave you my
 * address" and the next form. */
const char *auth_link_waiting(const struct auth_link *al);

/* True once, just after a registration completes. */
bool auth_link_registered(struct auth_link *al);

/* Queue application bytes. Returns AUTH_LINK_OK, or AUTH_LINK_ERR if the link
 * is not authenticated yet or the send window stayed full. */
int auth_link_send(struct auth_link *al, const void *data, size_t len);

/* True once authentication has succeeded and bytes may flow. */
bool auth_link_up(const struct auth_link *al);

/* The authenticated peer name, or an empty string. */
const char *auth_link_user(const struct auth_link *al);

void auth_link_close(struct auth_link *al);

#endif /* AUTH_LINK_H */
