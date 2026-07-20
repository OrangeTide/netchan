/* test_secure_link.c : encrypted echo round-trip over real loopback sockets */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * Drives a server and a client secure_link through a single iox loop on
 * 127.0.0.1. The client sends one message when the link comes up; the server
 * echoes it; the client asserts the bytes match and stops the loop. A watchdog
 * timer fails the test if the whole exchange does not finish in time.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "iox_loop.h"
#include "iox_timer.h"
#include "iox_fd.h"
#include "secure_link.h"
#include "nc_udp.h"

static const char MSG[] = "the quick brown fox";

struct endpoint {
    struct secure_link *sl;
    struct iox_loop    *loop;
    int                 got_echo;
    int                 sent;
};

static int
udp_ephemeral(int *port_out)
{
    struct sockaddr_in sa;
    socklen_t slen = sizeof(sa);
    int fd, fl;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        getsockname(fd, (struct sockaddr *)&sa, &slen) < 0) {
        close(fd);
        return -1;
    }
    fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
        (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    *port_out = ntohs(sa.sin_port);
    return fd;
}

/* server: echo whatever arrives. */
static void
srv_data(struct secure_link *sl, const uint8_t *d, size_t n, void *user)
{
    (void)user;
    (void)secure_link_send(sl, d, n);
}

/* client: send the message once the link is up. */
static void
cli_up(struct secure_link *sl, void *user)
{
    struct endpoint *c = user;

    if (!c->sent) {
        c->sent = 1;
        (void)secure_link_send(sl, MSG, sizeof(MSG) - 1);
    }
}

/* client: verify the echo and stop. */
static void
cli_data(struct secure_link *sl, const uint8_t *d, size_t n, void *user)
{
    struct endpoint *c = user;

    (void)sl;
    if (n == sizeof(MSG) - 1 && memcmp(d, MSG, n) == 0)
        c->got_echo = 1;
    iox_loop_stop(c->loop);
}

static void
watchdog(struct iox_loop *loop, void *arg)
{
    (void)arg;
    fprintf(stderr, "FAIL: timed out waiting for echo\n");
    iox_loop_stop(loop);
}

/****************************************************************
 * The tick has to keep ticking
 ****************************************************************/

static int hello_count;

/* A bare socket that answers nothing, so the only thing that can produce a
 * second datagram is the client's own periodic timer. */
static void
count_datagrams(struct iox_loop *l, int fd, unsigned events, void *arg)
{
    uint8_t buf[2048];

    (void)l;
    (void)events;
    (void)arg;
    while (recv(fd, buf, sizeof(buf), 0) > 0)
        hello_count++;
}

static void
stop_now(struct iox_loop *l, void *arg)
{
    (void)arg;
    iox_loop_stop(l);
}

/*
 * A client whose peer never replies must go on repeating its HELLO. This is
 * the regression test for a timer that was scheduled once and then quietly
 * retired, which left every session with no clock: no retransmissions, no
 * repeated handshake, and no way to notice a peer that had gone away. The
 * failure is invisible in any test where both ends answer promptly, which is
 * exactly why the round trip above passed while it was broken.
 */
static int
test_tick_keeps_running(void)
{
    struct iox_loop *loop;
    struct secure_link_cb ccb;
    struct secure_link *client;
    struct nc_addr peer;
    struct sockaddr_in sa;
    const uint8_t psk[32] = { 'd', 'e', 'm', 'o', 0 };
    int sfd, cfd, sport, fl;

    hello_count = 0;
    sfd = udp_ephemeral(&sport);
    cfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd < 0 || cfd < 0) {
        fprintf(stderr, "FAIL: tick test sockets\n");
        return -1;
    }
    fl = fcntl(cfd, F_GETFL, 0);
    if (fl >= 0)
        (void)fcntl(cfd, F_SETFL, fl | O_NONBLOCK);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)sport);
    if (nc_udp_from_sockaddr(&peer, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "FAIL: tick test address\n");
        return -1;
    }

    loop = iox_loop_new();
    iox_fd_add(loop, sfd, IOX_READ, count_datagrams, NULL);

    memset(&ccb, 0, sizeof(ccb));
    client = secure_link_open(loop, cfd, 0, &peer, psk, 1, &ccb);
    if (!client) {
        fprintf(stderr, "FAIL: tick test secure_link_open\n");
        return -1;
    }

    iox_timer_add(loop, 400, stop_now, NULL);
    iox_loop_run(loop);

    secure_link_close(client);
    iox_fd_remove(loop, sfd);
    iox_loop_free(loop);
    close(sfd);
    close(cfd);

    /* 400ms at a 50ms tick is about eight chances to repeat. Anything past
     * the first datagram proves the timer survived its own first firing. */
    if (hello_count < 3) {
        fprintf(stderr, "FAIL: only %d datagram(s) sent to a silent peer\n",
                hello_count);
        return -1;
    }
    return 0;
}

int
main(void)
{
    struct iox_loop *loop;
    struct secure_link_cb scb, ccb;
    struct endpoint server, client;
    struct nc_addr peer;
    struct sockaddr_in sa;
    const uint8_t psk[32] = { 'd', 'e', 'm', 'o', 0 };
    int sfd, cfd, sport, cport, fl;

    loop = iox_loop_new();
    if (!loop) {
        fprintf(stderr, "FAIL: iox_loop_new\n");
        return 1;
    }

    sfd = udp_ephemeral(&sport);
    if (sfd < 0) {
        fprintf(stderr, "FAIL: server socket\n");
        return 1;
    }
    cfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (cfd < 0) {
        fprintf(stderr, "FAIL: client socket\n");
        return 1;
    }
    fl = fcntl(cfd, F_GETFL, 0);
    if (fl >= 0)
        (void)fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
    (void)cport;

    /* build the server's address as an nc_addr for the client to target. */
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)sport);
    if (nc_udp_from_sockaddr(&peer, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "FAIL: nc_udp_from_sockaddr\n");
        return 1;
    }

    memset(&server, 0, sizeof(server));
    memset(&client, 0, sizeof(client));
    server.loop = client.loop = loop;

    memset(&scb, 0, sizeof(scb));
    scb.on_data = srv_data;
    scb.user = &server;

    memset(&ccb, 0, sizeof(ccb));
    ccb.on_up = cli_up;
    ccb.on_data = cli_data;
    ccb.user = &client;

    server.sl = secure_link_open(loop, sfd, 1, NULL, psk, 1, &scb);
    client.sl = secure_link_open(loop, cfd, 0, &peer, psk, 1, &ccb);
    if (!server.sl || !client.sl) {
        fprintf(stderr, "FAIL: secure_link_open\n");
        return 1;
    }

    iox_timer_add(loop, 3000, watchdog, NULL);
    iox_loop_run(loop);

    secure_link_close(server.sl);
    secure_link_close(client.sl);
    close(sfd);
    close(cfd);
    iox_loop_free(loop);

    if (!client.got_echo) {
        fprintf(stderr, "FAIL: echo mismatch or missing\n");
        return 1;
    }
    printf("ok: encrypted echo round-trip (\"%s\")\n", MSG);

    if (test_tick_keeps_running() != 0)
        return 1;
    printf("ok: a silent peer gets the handshake repeated\n");
    return 0;
}
