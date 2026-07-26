/* sockutil.h : bind and resolve helpers for the demo programs */

#ifndef SU_SOCKUTIL_H
#define SU_SOCKUTIL_H

#define SU_OK  (0)
#define SU_ERR (-1)

struct nc_addr;

/* Bind a non-blocking UDP socket. host may be NULL for the wildcard address,
 * port may be 0 to let the kernel choose. Returns the fd, or SU_ERR. */
int su_udp_bind(const char *host, int port);

/* Resolve host:port into an nc_addr. Returns SU_OK on success. */
int su_resolve(const char *host, int port, struct nc_addr *out);

/* The port a bound socket actually got, or SU_ERR. */
int su_local_port(int fd);

#endif /* SU_SOCKUTIL_H */
