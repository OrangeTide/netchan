/* test_nc_auth.c : the login conversation, both sides, no transport */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include <stdio.h>
#include <string.h>

#include "nc_auth.h"
#include "monocypher.h"

/*
 * Both state machines run in one process with a message queue between them.
 * That is the payoff for keeping nc_auth ignorant of its transport: the
 * interesting cases, a replayed signature or an unauthorised key, need no
 * sockets, no timing, and no second process to reproduce.
 */

#define MAX_Q 32

struct wire {
    struct {
        int     to_server;
        uint8_t buf[NC_AUTH_MAX_MSG];
        size_t  len;
    } q[MAX_Q];
    int head, tail;
    struct nc_auth *client, *server;
};

static struct wire W;

static void service_needs(void);

static void
push(int to_server, const void *msg, size_t len)
{
    if (W.tail >= MAX_Q)
        return;
    W.q[W.tail].to_server = to_server;
    memcpy(W.q[W.tail].buf, msg, len);
    W.q[W.tail].len = len;
    W.tail++;
}

static void send_to_server(void *ctx, const void *m, size_t n) { (void)ctx; push(1, m, n); }
static void send_to_client(void *ctx, const void *m, size_t n) { (void)ctx; push(0, m, n); }

/* Deliver until nobody has anything left to say. */
static void
pump(void)
{
    while (W.head < W.tail) {
        int to_server = W.q[W.head].to_server;
        uint8_t buf[NC_AUTH_MAX_MSG];
        size_t len = W.q[W.head].len;

        memcpy(buf, W.q[W.head].buf, len);
        W.head++;
        nc_auth_feed(to_server ? W.server : W.client, buf, len);
        service_needs();
    }
    W.head = W.tail = 0;
}

/****************************************************************
 * Fixtures
 ****************************************************************/

static uint8_t good_sk[64], good_pk[32];
static int     client_has_key;
static const char *client_password = "correct horse";
static const char *server_password = "correct horse";
static char    server_saw[NC_AUTH_MAX_USER + 1];

/*
 * Stand in for the application that answers a credential request. A real
 * client takes an unbounded amount of time here; the test answers at once,
 * which exercises the same suspend-and-resume path either way.
 */
static void
service_needs(void)
{
    int guard;

    if (!W.client)
        return;

    /* Declining one method makes the next one wanted straight away, so keep
     * answering until the conversation stops asking. */
    for (guard = 0; guard < 8; guard++) {
        int need = nc_auth_needs(W.client);

        if (need == NC_AUTH_NEED_KEY) {
            if (client_has_key)
                nc_auth_supply_key(W.client, good_sk, good_pk);
            else
                nc_auth_supply_key(W.client, NULL, NULL);
        } else if (need == NC_AUTH_NEED_PASSWORD) {
            nc_auth_supply_password(W.client, client_password);
        } else {
            return;
        }
    }
}

static unsigned
s_methods(void *ctx, const char *user)
{
    (void)ctx;
    (void)user;
    return NC_AUTH_M_PUBKEY | NC_AUTH_M_PASSWORD;
}

static int
s_check_key(void *ctx, const char *user, const uint8_t pk[32])
{
    (void)ctx;
    return strcmp(user, "alice") == 0 && crypto_verify32(pk, good_pk) == 0;
}

static int
s_check_password(void *ctx, const char *user, const char *password)
{
    (void)ctx;
    return strcmp(user, "alice") == 0 && strcmp(password, server_password) == 0;
}

/****************************************************************
 * Cases
 ****************************************************************/

static int failures;

static void
check(const char *what, int cond)
{
    printf("%s: %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        failures++;
}

/* Run one whole conversation over a given session id and return the two
 * final states. */
static void
run(const uint8_t sid[32], const char *user, int *cstate, int *sstate)
{
    static struct nc_auth client, server;
    struct nc_auth_server_cb scb;

    memset(&scb, 0, sizeof(scb));
    scb.methods = s_methods;
    scb.check_key = s_check_key;
    scb.check_password = s_check_password;

    memset(&W, 0, sizeof(W));
    W.client = &client;
    W.server = &server;

    nc_auth_client_init(&client, sid, user, send_to_server, NULL);
    nc_auth_server_init(&server, sid, &scb, send_to_client, NULL);

    nc_auth_start(&client);
    pump();

    *cstate = nc_auth_state(&client);
    *sstate = nc_auth_state(&server);
    snprintf(server_saw, sizeof(server_saw), "%s", nc_auth_user(&server));
}

int
main(void)
{
    uint8_t sid_a[32], sid_b[32], seed[32];
    int cs, ss;

    memset(sid_a, 0xa1, sizeof(sid_a));
    memset(sid_b, 0xb2, sizeof(sid_b));

    memset(seed, 1, sizeof(seed));
    crypto_eddsa_key_pair(good_sk, good_pk, seed);

    /* 1. Public key on file: instant login, password never requested. */
    client_has_key = 1;
    run(sid_a, "alice", &cs, &ss);
    check("public key logs in", cs == NC_AUTH_OK && ss == NC_AUTH_OK);
    check("server learned the name", strcmp(server_saw, "alice") == 0);

    /* 2. No key enrolled: the client falls back and the password works. */
    client_has_key = 0;
    client_password = "correct horse";
    run(sid_a, "alice", &cs, &ss);
    check("password fallback logs in", cs == NC_AUTH_OK && ss == NC_AUTH_OK);

    /* 3. Wrong password: denied, and no method is left to retry. */
    client_password = "hunter2";
    run(sid_a, "alice", &cs, &ss);
    check("wrong password is denied",
          cs == NC_AUTH_DENIED && ss != NC_AUTH_OK);

    /* 4. A key that is not on the server's list, for a name that is. */
    client_has_key = 1;
    client_password = "hunter2";
    run(sid_a, "mallory", &cs, &ss);
    check("unauthorised name is denied",
          cs == NC_AUTH_DENIED && ss != NC_AUTH_OK);

    /* 5. The session binding. Capture the PUBKEY message from a successful
     *    conversation and replay it into a server that has a different
     *    session id. This is the exact move a malicious relay would make,
     *    and the signature must not verify. */
    {
        static struct nc_auth client, server2;
        struct nc_auth_server_cb scb;
        uint8_t captured[NC_AUTH_MAX_MSG];
        size_t captured_len = 0;
        int i;

        memset(&scb, 0, sizeof(scb));
        scb.methods = s_methods;
        scb.check_key = s_check_key;
        scb.check_password = s_check_password;

        client_has_key = 1;
        client_password = "correct horse";

        /* A live session on sid_a, from which we steal the signature. */
        memset(&W, 0, sizeof(W));
        W.client = &client;
        W.server = &server2;
        nc_auth_client_init(&client, sid_a, "alice", send_to_server, NULL);
        nc_auth_server_init(&server2, sid_a, &scb, send_to_client, NULL);
        nc_auth_start(&client);

        /* Walk the queue by hand so the PUBKEY message can be copied out. */
        while (W.head < W.tail) {
            i = W.head++;
            if (W.q[i].to_server && W.q[i].buf[0] == 0x12) {
                memcpy(captured, W.q[i].buf, W.q[i].len);
                captured_len = W.q[i].len;
            }
            nc_auth_feed(W.q[i].to_server ? W.server : W.client,
                         W.q[i].buf, W.q[i].len);
            service_needs();
        }
        check("captured a signature to replay", captured_len > 0);

        /* Replay it at a server whose session id is different. */
        memset(&W, 0, sizeof(W));
        W.client = &client;
        W.server = &server2;
        nc_auth_server_init(&server2, sid_b, &scb, send_to_client, NULL);
        {
            uint8_t hello[8];

            hello[0] = 0x10;
            hello[1] = 5;
            memcpy(hello + 2, "alice", 5);
            nc_auth_feed(&server2, hello, 7);
        }
        nc_auth_feed(&server2, captured, captured_len);
        check("replayed signature is rejected",
              nc_auth_state(&server2) != NC_AUTH_OK);
    }

    if (failures)
        printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
