/* mc_ipx.h : 16-bit MS-DOS IPX transport for microchan (polling, no ESR) */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef MC_IPX_H
#define MC_IPX_H

#include <stddef.h>
#include <stdint.h>
#include "mc_addr.h"
#include "microchan.h"        /* for MC_MTU */

/*
 * Reaches the real-mode IPX driver through the standard far-call entry
 * point obtained with INT 2Fh AX=7A00h (Novell IPX/SPX, also emulated by
 * DOSBox). Receive uses a pool of pre-posted "listen" ECBs that the caller
 * polls; there is no Event Service Routine.
 *
 * All ECBs and packet buffers live in one conventional-memory block so the
 * driver can address them by real-mode segment:offset. mc_addr packs an IPX
 * address as network(4) + node(6) + socket(2), big-endian, len = 12.
 */

/* Tunables: pool depths. Each buffer is 30 (IPX header) + MC_MTU bytes. */
#ifndef MC_IPX_RECV
#define MC_IPX_RECV 6
#endif
#ifndef MC_IPX_SEND
#define MC_IPX_SEND 4
#endif

struct mc_ipx {
    uint16_t socket;        /* our socket number (host order)              */
    uint8_t  net[4];        /* local network number                        */
    uint8_t  node[6];       /* local node (MAC) address                    */
    uint16_t next_recv;     /* round-robin poll cursor                     */
};

/** Probe for the IPX driver. Returns 1 if present, 0 otherwise. */
int mc_ipx_available(void);

/** Open the socket and post the listen pool. Returns 0 on success, -1 on
 *  failure (no driver, out of memory, or socket open error). */
int mc_ipx_open(struct mc_ipx *x, unsigned socket);

void mc_ipx_close(struct mc_ipx *x);

/** Poll for one received datagram. Returns length, 0 if none, -1 on error.
 *  The IPX payload (after the 30-byte header) is copied into buf. */
int mc_ipx_recv(struct mc_ipx *x, void *buf, size_t buflen,
                struct mc_addr *from);

/** Send one datagram to an IPX address. Returns bytes sent, or -1/0. */
int mc_ipx_send(struct mc_ipx *x, const void *buf, size_t len,
                const struct mc_addr *to);

/** Our own IPX address on the given socket. */
void mc_ipx_local(struct mc_ipx *x, struct mc_addr *out);

/** The broadcast address (node FF:FF:FF:FF:FF:FF) on the given socket. */
void mc_ipx_broadcast(struct mc_ipx *x, struct mc_addr *out);

#endif /* MC_IPX_H */
