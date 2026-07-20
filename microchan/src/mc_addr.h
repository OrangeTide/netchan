/* mc_addr.h : transport-agnostic network address for microchan */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef MC_ADDR_H
#define MC_ADDR_H

#include <stdint.h>

/*
 * An opaque transport address. The core never interprets the bytes; each
 * transport packs its own address into a[] and sets len. Sized for the
 * largest transport we support:
 *   IPX  net(4) + node(6) + socket(2) = 12 bytes
 *   UDP  ip(4) + port(2)              =  6 bytes
 * Addresses are compared and copied bytewise over the first len bytes.
 */
#define MC_ADDR_MAX 12

struct mc_addr {
    uint8_t len;
    uint8_t a[MC_ADDR_MAX];
};

#endif /* MC_ADDR_H */
