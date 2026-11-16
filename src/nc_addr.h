/* nc_addr.h : opaque transport address for netchan */

#ifndef NC_ADDR_H
#define NC_ADDR_H

#include <stdint.h>
#include <stdbool.h>

/*
 * A transport-agnostic address. The netchan core never interprets the
 * bytes in a[]; it only copies them and compares them for equality. Each
 * transport backend decides how to pack its own addressing into a[]:
 *
 *   nc_udp   packs an IPv4 or IPv6 address plus port (see nc_udp.h).
 *   others   may pack a channel handle, a peer index, whatever fits.
 *
 * This is the single seam that lets one protocol core run over UDP on the
 * desktop and over WebRTC or WebSockets in a browser without the core
 * knowing which. 19 bytes is enough for the largest packing we ship
 * (IPv6 + port); a[] is sized with a little headroom.
 */

#define NC_ADDR_MAX 20

struct nc_addr {
    uint8_t len;              /* bytes used in a[]; 0 means "unset" */
    uint8_t a[NC_ADDR_MAX];
};

/*
 * The one definition of how an IPv4 address and port pack into nc_addr:
 * a[0] is the family tag 4, a[1..4] the address in network order, a[5..6]
 * the port big-endian, and len is 7. nc_udp copies the same bytes to and
 * from a sockaddr, and a CONNECT_REDIRECT frame carries them unchanged, so
 * a value one of them produces is one the others read. These accessors are
 * for the places that hold a host-order integer instead of those bytes:
 * a STUN reply and a room code.
 */

static inline void
nc_addr_set_ipv4(struct nc_addr *a, uint32_t ip, uint16_t port)
{
    a->len = 7;
    a->a[0] = 4;
    a->a[1] = (uint8_t)(ip >> 24);
    a->a[2] = (uint8_t)(ip >> 16);
    a->a[3] = (uint8_t)(ip >> 8);
    a->a[4] = (uint8_t)ip;
    a->a[5] = (uint8_t)(port >> 8);
    a->a[6] = (uint8_t)port;
}

/* Reads an IPv4 nc_addr back into a host-order address and port. Returns
 * false for an unset address or any other family, leaving the outputs
 * untouched. */
static inline bool
nc_addr_get_ipv4(const struct nc_addr *a, uint32_t *ip, uint16_t *port)
{
    if (a->len != 7 || a->a[0] != 4)
        return false;
    *ip = (uint32_t)a->a[1] << 24 | (uint32_t)a->a[2] << 16
        | (uint32_t)a->a[3] << 8 | a->a[4];
    *port = (uint16_t)((uint16_t)a->a[5] << 8 | a->a[6]);
    return true;
}

#endif /* NC_ADDR_H */
