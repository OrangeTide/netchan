/* sockutil.h : bind and resolve helpers for the demo programs */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef SOCKUTIL_H
#define SOCKUTIL_H

struct nc_addr;

/* Bind a non-blocking UDP socket. host may be NULL for the wildcard address,
 * port may be 0 to let the kernel choose. Returns the fd, or -1. */
int su_udp_bind(const char *host, int port);

/* Resolve host:port into an nc_addr. Returns 0 on success. */
int su_resolve(const char *host, int port, struct nc_addr *out);

/* The port a bound socket actually got, or -1. */
int su_local_port(int fd);

#endif /* SOCKUTIL_H */
