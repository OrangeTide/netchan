/* mc_udp.h : host (POSIX) UDP transport for microchan development */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef MC_UDP_H
#define MC_UDP_H

#include <stddef.h>
#include <stdint.h>
#include "mc_addr.h"

struct mc_udp {
    int fd;
};

/** Open a non-blocking UDP socket. bind_ip may be NULL (any); port 0 picks
 *  an ephemeral port. Returns NC-style 0 on success, -1 on failure. */
int mc_udp_open(struct mc_udp *u, const char *bind_ip, uint16_t port);

void mc_udp_close(struct mc_udp *u);

/** Receive one datagram. Returns length, 0 if none pending, -1 on error. */
int mc_udp_recv(struct mc_udp *u, void *buf, size_t buflen,
                struct mc_addr *from);

/** Send one datagram. Returns bytes sent or -1. */
int mc_udp_send(struct mc_udp *u, const void *buf, size_t len,
                const struct mc_addr *to);

/** Pack a dotted-quad host and port into an mc_addr. Returns 0 or -1. */
int mc_udp_addr(const char *ip, uint16_t port, struct mc_addr *out);

/** The local address actually bound (after an ephemeral bind). */
int mc_udp_local(struct mc_udp *u, struct mc_addr *out);

#endif /* MC_UDP_H */
