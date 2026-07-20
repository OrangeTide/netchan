/* echo_client.c : encrypted netchan echo client over the iox event loop */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "iox_loop.h"
#include "iox_fd.h"
#include "iox_signal.h"
#include "iox_timer.h"
#include "nc_udp.h"
#include "secure_link.h"
#include "monocypher.h"

struct client {
    struct iox_loop    *loop;
    struct secure_link *sl;
    int                 stdin_watched;
};

static void
derive_psk(uint8_t out[32], const char *phrase)
{
    crypto_blake2b(out, 32, (const uint8_t *)phrase, strlen(phrase));
}

static void
linger_done(struct iox_loop *loop, void *arg)
{
    (void)arg;
    iox_loop_stop(loop);
}

static void
on_stdin(struct iox_loop *loop, int fd, unsigned events, void *arg)
{
    struct client *c = arg;
    char buf[1024];
    ssize_t r;

    (void)events;
    r = read(fd, buf, sizeof(buf));
    if (r <= 0) {
        /* EOF on the keyboard: stop reading, but linger briefly so any
         * echo still in flight can arrive before we exit. */
        iox_fd_remove(loop, fd);
        c->stdin_watched = 0;
        iox_timer_add(loop, 400, linger_done, NULL);
        return;
    }
    if (secure_link_send(c->sl, buf, (size_t)r) != 0)
        fprintf(stderr, "[client] send failed\n");
}

static void
on_up(struct secure_link *sl, void *user)
{
    struct client *c = user;

    (void)sl;
    fprintf(stderr, "[client] session up -- type a line and see it echoed\n");
    /* Only start reading the keyboard once the link can carry bytes. */
    if (!c->stdin_watched &&
        iox_fd_add(c->loop, STDIN_FILENO, IOX_READ, on_stdin, c) == 0)
        c->stdin_watched = 1;
}

static void
on_data(struct secure_link *sl, const uint8_t *data, size_t len, void *user)
{
    (void)sl;
    (void)user;
    fputs("[echo] ", stdout);
    fwrite(data, 1, len, stdout);
    fflush(stdout);
}

static void
on_down(struct secure_link *sl, void *user)
{
    struct client *c = user;

    (void)sl;
    fprintf(stderr, "[client] server disconnected\n");
    iox_loop_stop(c->loop);
}

static void
on_signal(struct iox_loop *loop, int signo, void *arg)
{
    (void)signo;
    (void)arg;
    iox_loop_stop(loop);
}

/* Resolve host/port to an nc_addr via the UDP backend's packer. */
static int
resolve(const char *host, const char *port, struct nc_addr *out, int *family)
{
    struct addrinfo hints, *res;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0)
        return -1;
    *family = res->ai_family;
    rc = nc_udp_from_sockaddr(out, res->ai_addr, (socklen_t)res->ai_addrlen);
    freeaddrinfo(res);
    return rc;
}

int
main(int argc, char **argv)
{
    struct iox_loop *loop;
    struct secure_link_cb cb;
    struct client c;
    struct nc_addr peer;
    uint8_t psk[32];
    const uint8_t *pskp = NULL;
    const char *host = "127.0.0.1";
    const char *port = "9000";
    int use_crypto = 1, family = AF_INET;
    int fd, fl, i, nposi = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--plain") == 0) {
            use_crypto = 0;
        } else if (strcmp(argv[i], "--psk") == 0 && i + 1 < argc) {
            derive_psk(psk, argv[++i]);
            pskp = psk;
        } else if (nposi == 0) {
            host = argv[i];
            nposi++;
        } else {
            port = argv[i];
            nposi++;
        }
    }

    if (resolve(host, port, &peer, &family) != 0) {
        fprintf(stderr, "cannot resolve %s:%s\n", host, port);
        return 1;
    }

    fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
        (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    loop = iox_loop_new();
    if (!loop) {
        fprintf(stderr, "iox_loop_new failed\n");
        return 1;
    }
    iox_signal_add(loop, SIGINT, on_signal, NULL);

    memset(&c, 0, sizeof(c));
    c.loop = loop;

    memset(&cb, 0, sizeof(cb));
    cb.on_up = on_up;
    cb.on_data = on_data;
    cb.on_down = on_down;
    cb.user = &c;

    c.sl = secure_link_open(loop, fd, 0, &peer, pskp, use_crypto, &cb);
    if (!c.sl) {
        fprintf(stderr, "secure_link_open failed\n");
        return 1;
    }

    fprintf(stderr, "[client] connecting to %s:%s (%s)\n", host, port,
            use_crypto ? (pskp ? "encrypted + psk" : "encrypted") : "plaintext");

    iox_loop_run(loop);

    secure_link_close(c.sl);
    close(fd);
    iox_loop_free(loop);
    return 0;
}
