/* secure_link.h : encrypted netchan session driven by the iox event loop */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef SECURE_LINK_H
#define SECURE_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "nc_addr.h"
#include "nc_crypto.h"

struct iox_loop;
struct netchan_conn;
struct netchan_chan;

/*
 * secure_link wires three things the rest of the demo never has to see:
 *
 *   - a netchan connection (the reliable-datagram protocol core),
 *   - an optional nc_crypto decorator (X25519 handshake + per-packet AEAD),
 *   - an iox event loop that owns the UDP socket, a retransmit timer, and
 *     the process signals.
 *
 * The caller supplies callbacks. on_up fires once the session is fully
 * established and application bytes may flow; on_data delivers a run of
 * received bytes; on_down fires when the peer disconnects. None of the
 * callbacks are required.
 *
 * The crypto handshake, when enabled, always completes before netchan's own
 * connect handshake, so every netchan datagram (including its SYN) travels
 * sealed. That ordering is the whole reason nc_crypto can stay a transport
 * decorator the protocol core knows nothing about.
 */

struct secure_link;

typedef void (*sl_up_cb)(struct secure_link *sl, void *user);
typedef void (*sl_data_cb)(struct secure_link *sl,
                           const uint8_t *data, size_t len, void *user);
typedef void (*sl_down_cb)(struct secure_link *sl, void *user);

struct secure_link_cb {
    sl_up_cb   on_up;
    sl_data_cb on_data;
    sl_down_cb on_down;
    void      *user;
};

/*
 * Create a session over an already-bound, non-blocking UDP socket fd.
 *
 * server is 1 for the accepting side, 0 for the connecting side. A client
 * must pass peer (the server address); a server passes NULL and learns its
 * peer from the first datagram. psk, if non-NULL, is a 32-byte pre-shared
 * key; pass NULL for an unauthenticated ephemeral handshake. use_crypto
 * selects whether the nc_crypto decorator is inserted at all.
 *
 * The link registers its own fd watcher and a periodic service timer on the
 * loop. Returns NULL on failure.
 */
struct secure_link *secure_link_open(struct iox_loop *loop, int fd, int server,
                                     const struct nc_addr *peer,
                                     const uint8_t *psk, int use_crypto,
                                     const struct secure_link_cb *cb);

/* Queue application bytes on the reliable channel. Returns 0 on success,
 * -1 if the link is not up yet or the send window stayed full. */
int secure_link_send(struct secure_link *sl, const void *data, size_t len);

/* Non-zero once the session is established and secure_link_send may be used. */
int secure_link_up(const struct secure_link *sl);

/* Tear down the session and release the socket. */
void secure_link_close(struct secure_link *sl);

#endif /* SECURE_LINK_H */
