/* auth_link.h : authenticated encrypted netchan session on the iox loop */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef AUTH_LINK_H
#define AUTH_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "nc_addr.h"
#include "nc_crypto.h"
#include "nc_auth.h"

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
 * The login needs a credential: NC_AUTH_NEED_KEY or NC_AUTH_NEED_PASSWORD.
 * Nothing is waiting on the answer, so the handler is free to return
 * immediately and supply it later, which is how a client prompts a human
 * without stalling the loop the session depends on.
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

/* Queue application bytes. Returns 0, or -1 if the link is not authenticated
 * yet or the send window stayed full. */
int auth_link_send(struct auth_link *al, const void *data, size_t len);

/* Non-zero once authentication has succeeded and bytes may flow. */
int auth_link_up(const struct auth_link *al);

/* The authenticated peer name, or an empty string. */
const char *auth_link_user(const struct auth_link *al);

void auth_link_close(struct auth_link *al);

#endif /* AUTH_LINK_H */
