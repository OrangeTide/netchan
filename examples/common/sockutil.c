/* sockutil.c : bind and resolve helpers for the demo programs */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "sockutil.h"

#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "nc_addr.h"
#include "nc_udp.h"

static int
set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);

    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int
su_udp_bind(const char *host, int port)
{
    struct addrinfo hints, *res = NULL, *ai;
    char service[16];
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = host ? 0 : AI_PASSIVE;
    snprintf(service, sizeof(service), "%d", port);

    if (getaddrinfo(host, service, &hints, &res) != 0)
        return -1;

    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
            set_nonblock(fd) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int
su_resolve(const char *host, int port, struct nc_addr *out)
{
    struct addrinfo hints, *res = NULL;
    char service[16];
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(service, sizeof(service), "%d", port);

    if (getaddrinfo(host, service, &hints, &res) != 0)
        return -1;
    rc = nc_udp_from_sockaddr(out, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    return rc;
}

int
su_local_port(int fd)
{
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);

    if (getsockname(fd, (struct sockaddr *)&ss, &slen) != 0)
        return -1;
    if (ss.ss_family == AF_INET)
        return ntohs(((struct sockaddr_in *)&ss)->sin_port);
    if (ss.ss_family == AF_INET6)
        return ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    return -1;
}
