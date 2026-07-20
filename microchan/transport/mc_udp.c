/* mc_udp.c : host (POSIX) UDP transport for microchan development */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "mc_udp.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/*
 * mc_addr packing for UDP/IPv4: 4 address bytes (network order) followed by
 * 2 port bytes (network order), len = 6.
 */

static void
addr_from_sin(struct mc_addr *a, const struct sockaddr_in *sin)
{
    a->len = 6;
    memcpy(a->a, &sin->sin_addr.s_addr, 4);
    memcpy(a->a + 4, &sin->sin_port, 2);
}

static void
sin_from_addr(struct sockaddr_in *sin, const struct mc_addr *a)
{
    memset(sin, 0, sizeof(*sin));
    sin->sin_family = AF_INET;
    memcpy(&sin->sin_addr.s_addr, a->a, 4);
    memcpy(&sin->sin_port, a->a + 4, 2);
}

int
mc_udp_open(struct mc_udp *u, const char *bind_ip, uint16_t port)
{
    struct sockaddr_in sin;
    int fd, fl;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = bind_ip ? inet_addr(bind_ip) : htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        close(fd);
        return -1;
    }

    fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    u->fd = fd;
    return 0;
}

void
mc_udp_close(struct mc_udp *u)
{
    if (u && u->fd >= 0) {
        close(u->fd);
        u->fd = -1;
    }
}

int
mc_udp_recv(struct mc_udp *u, void *buf, size_t buflen, struct mc_addr *from)
{
    struct sockaddr_in sin;
    socklen_t sl = sizeof(sin);
    ssize_t n;

    n = recvfrom(u->fd, buf, buflen, 0, (struct sockaddr *)&sin, &sl);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    if (from)
        addr_from_sin(from, &sin);
    return (int)n;
}

int
mc_udp_send(struct mc_udp *u, const void *buf, size_t len,
            const struct mc_addr *to)
{
    struct sockaddr_in sin;
    ssize_t n;

    sin_from_addr(&sin, to);
    n = sendto(u->fd, buf, len, 0, (struct sockaddr *)&sin, sizeof(sin));
    return (int)n;
}

int
mc_udp_addr(const char *ip, uint16_t port, struct mc_addr *out)
{
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = inet_addr(ip);
    if (sin.sin_addr.s_addr == INADDR_NONE && strcmp(ip, "255.255.255.255"))
        return -1;
    addr_from_sin(out, &sin);
    return 0;
}

int
mc_udp_local(struct mc_udp *u, struct mc_addr *out)
{
    struct sockaddr_in sin;
    socklen_t sl = sizeof(sin);
    if (getsockname(u->fd, (struct sockaddr *)&sin, &sl) < 0)
        return -1;
    addr_from_sin(out, &sin);
    return 0;
}
