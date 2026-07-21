/* mc_memlink.h : an in-memory datagram link for microchan tests */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * The microchan core and the game net layer are both buffer-in, buffer-out:
 * they never touch a socket. Driving them over loopback UDP therefore tests
 * nothing about them, and it drags in the host's delivery timing, which is
 * synchronous on Linux and not on macOS.
 *
 * This is the transport those tests actually want. Delivery is a function
 * call, so it is immediate, identical everywhere, and free. It also makes
 * the impairments explicit: loss, duplication, and reordering happen on a
 * fixed count rather than by luck, which is what a Go-Back-N window needs
 * exercised. mc_udp still has its own test for the socket work.
 */

#ifndef MC_MEMLINK_H
#define MC_MEMLINK_H

#include <stddef.h>
#include <stdint.h>
#include "mc_addr.h"
#include "microchan.h"

#define MEML_MAX_EP  4          /* endpoints on one link                   */
#define MEML_MAX_Q   64         /* datagrams queued per endpoint           */

struct meml_pkt {
    uint8_t buf[MC_MTU];
    size_t len;
    struct mc_addr from;
};

struct meml_ep {
    struct mc_addr addr;
    struct meml_pkt q[MEML_MAX_Q];
    int head, count;
    struct meml_pkt held;       /* one datagram pulled out of order        */
    int have_held;
    int used;
};

struct memlink {
    struct meml_ep ep[MEML_MAX_EP];

    /* Impairments, counted over every datagram offered to the link.
     * 0 disables. Set them directly between phases of a test. */
    unsigned drop_every;        /* Nth datagram is discarded               */
    unsigned dup_every;         /* Nth datagram is delivered twice         */
    unsigned reorder_every;     /* Nth datagram yields to the one behind it */

    unsigned offered, delivered, dropped, duped, reordered;
};

/** Reset a link to empty with no impairments. */
void meml_init(struct memlink *l);

/** Claim an endpoint and write its address. Returns 0, or -1 if the link
 *  is full. */
int meml_open(struct memlink *l, struct mc_addr *out);

/** Offer one datagram. Impairments apply here, so the count a datagram is
 *  measured against is the order it was sent, not the order it lands.
 *  Returns 0 when the datagram was accepted, which includes being dropped
 *  on purpose, and -1 when the destination is unknown or its queue is
 *  full. */
int meml_send(struct memlink *l, const struct mc_addr *from,
              const void *buf, size_t len, const struct mc_addr *to);

/** Take the next datagram for an endpoint. Returns its length, 0 when
 *  none is waiting, or -1 if the endpoint is unknown. Mirrors
 *  mc_udp_recv() so a test reads the same either way. */
int meml_recv(struct memlink *l, const struct mc_addr *self,
              void *buf, size_t buflen, struct mc_addr *from);

#endif /* MC_MEMLINK_H */
