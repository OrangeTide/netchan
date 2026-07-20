/* echo_server.c : encrypted netchan echo server over the iox event loop */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "iox_loop.h"
#include "iox_signal.h"
#include "secure_link.h"
#include "monocypher.h"

/* Hash a passphrase into a 32-byte pre-shared key. A real deployment would
 * use a slow KDF (argon2); for a closed-LAN demo BLAKE2b is enough to show
 * the PSK path through nc_crypto. */
static void
derive_psk(uint8_t out[32], const char *phrase)
{
    crypto_blake2b(out, 32, (const uint8_t *)phrase, strlen(phrase));
}

static void
on_up(struct secure_link *sl, void *user)
{
    (void)sl;
    (void)user;
    fprintf(stderr, "[server] peer connected\n");
}

static void
on_data(struct secure_link *sl, const uint8_t *data, size_t len, void *user)
{
    (void)user;
    /* Echo the bytes straight back on our own reliable channel. */
    fprintf(stderr, "[server] echoing %zu bytes\n", len);
    (void)secure_link_send(sl, data, len);
}

static void
on_down(struct secure_link *sl, void *user)
{
    (void)sl;
    (void)user;
    fprintf(stderr, "[server] peer disconnected\n");
}

static void
on_signal(struct iox_loop *loop, int signo, void *arg)
{
    (void)signo;
    (void)arg;
    fprintf(stderr, "\n[server] shutting down\n");
    iox_loop_stop(loop);
}

static int
udp_bind(int port)
{
    struct sockaddr_in sa;
    int fd, fl, on = 1;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
        (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

int
main(int argc, char **argv)
{
    struct iox_loop *loop;
    struct secure_link *sl;
    struct secure_link_cb cb;
    uint8_t psk[32];
    const uint8_t *pskp = NULL;
    int use_crypto = 1;
    int port = 9000;
    int fd, i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--plain") == 0) {
            use_crypto = 0;
        } else if (strcmp(argv[i], "--psk") == 0 && i + 1 < argc) {
            derive_psk(psk, argv[++i]);
            pskp = psk;
        } else {
            port = atoi(argv[i]);
        }
    }

    fd = udp_bind(port);
    if (fd < 0) {
        perror("bind");
        return 1;
    }

    loop = iox_loop_new();
    if (!loop) {
        fprintf(stderr, "iox_loop_new failed\n");
        return 1;
    }
    iox_signal_add(loop, SIGINT, on_signal, NULL);
    iox_signal_add(loop, SIGTERM, on_signal, NULL);

    memset(&cb, 0, sizeof(cb));
    cb.on_up = on_up;
    cb.on_data = on_data;
    cb.on_down = on_down;

    sl = secure_link_open(loop, fd, 1, NULL, pskp, use_crypto, &cb);
    if (!sl) {
        fprintf(stderr, "secure_link_open failed\n");
        return 1;
    }

    fprintf(stderr, "[server] listening on udp/%d (%s)\n", port,
            use_crypto ? (pskp ? "encrypted + psk" : "encrypted") : "plaintext");

    iox_loop_run(loop);

    secure_link_close(sl);
    close(fd);
    iox_loop_free(loop);
    return 0;
}
