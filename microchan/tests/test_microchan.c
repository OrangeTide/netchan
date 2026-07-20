/* test_microchan.c : host loopback tests for the microchan core (over UDP) */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "microchan.h"
#include "mc_udp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>

static int g_fail = 0;

#define CHECK(cond, msg) do {                       \
        if (cond) {                                 \
            printf("  ok   %s\n", msg);             \
        } else {                                    \
            printf("  FAIL %s\n", msg);             \
            g_fail++;                               \
        }                                           \
    } while (0)

/* Deterministic drop: drop every g_drop'th datagram (0 = no loss). */
static unsigned g_seq;
static int g_drop;

static void
recv_pump(struct mc_udp *u, struct microchan *nc)
{
    uint8_t buf[MC_MTU];
    struct mc_addr from;
    int n;
    while ((n = mc_udp_recv(u, buf, sizeof(buf), &from)) > 0)
        mc_feed(nc, buf, (size_t)n, &from);
}

static void
send_pump(struct mc_udp *u, struct microchan *nc)
{
    uint8_t buf[MC_MTU];
    struct mc_addr to;
    int n;
    while ((n = (int)mc_send_next(nc, buf, sizeof(buf), &to)) > 0) {
        g_seq++;
        if (g_drop && (g_seq % (unsigned)g_drop) == 0)
            continue;           /* simulate packet loss */
        mc_udp_send(u, buf, (size_t)n, &to);
    }
}

/*
 * Wait for a datagram to become readable. The reliable tests reach their
 * goal through retransmission, so a late delivery only costs them an
 * iteration, but an unreliable datagram arrives once or not at all. Linux
 * queues loopback traffic inside sendto(), while macOS hands it to a
 * separate context, so a drain loop that never waits can finish before the
 * last datagram lands.
 */
static void
wait_pkt(struct mc_udp *u, int ms)
{
    struct pollfd p;

    p.fd = u->fd;
    p.events = POLLIN;
    p.revents = 0;
    poll(&p, 1, ms);
}

int
main(void)
{
    struct mc_udp su, cu;
    struct mc_addr saddr;
    struct microchan *s, *cl;
    struct mc_chan *s_rel, *s_unr, *c_rel, *c_unr;
    uint32_t now = 0;
    int i;

    srand(20260601u);

    if (mc_udp_open(&su, "127.0.0.1", 0) != 0 ||
        mc_udp_open(&cu, "127.0.0.1", 0) != 0) {
        printf("socket setup failed\n");
        return 2;
    }
    mc_udp_local(&su, &saddr);

    s = mc_open(1);
    cl = mc_open(0);
    s_rel = mc_chan_open(s, MC_RELIABLE);
    s_unr = mc_chan_open(s, MC_UNRELIABLE);
    c_rel = mc_chan_open(cl, MC_RELIABLE);
    c_unr = mc_chan_open(cl, MC_UNRELIABLE);

    printf("microchan core tests (UDP loopback)\n");
    printf("MTU=%d WINDOW=%d MAXMSG=%d\n", MC_MTU, MC_WINDOW, MC_MAXMSG);

    /* --- handshake --- */
    mc_connect(cl, &saddr);
    for (i = 0; i < 100; i++) {
        if (mc_state(s) == MC_STATE_CONNECTED &&
            mc_state(cl) == MC_STATE_CONNECTED)
            break;
        mc_service(s, now);
        mc_service(cl, now);
        recv_pump(&cu, cl);
        recv_pump(&su, s);
        send_pump(&cu, cl);
        send_pump(&su, s);
        now += 20;
    }
    CHECK(mc_state(cl) == MC_STATE_CONNECTED, "client connected");
    CHECK(mc_state(s) == MC_STATE_CONNECTED, "server connected");

    /* --- reliable in-order delivery under 25% packet loss --- */
    g_drop = 4;
    {
        const int N = 40;
        int sent = 0, recvd = 0, order_ok = 1;
        char rb[64];
        for (i = 0; i < 4000 && recvd < N; i++) {
            while (sent < N) {
                char m[16];
                int ml = sprintf(m, "R%03d", sent);
                if (mc_write(c_rel, m, (size_t)ml) == MC_ERR_AGAIN)
                    break;
                sent++;
            }
            mc_service(s, now);
            mc_service(cl, now);
            recv_pump(&cu, cl);
            recv_pump(&su, s);
            send_pump(&cu, cl);
            send_pump(&su, s);
            {
                int n;
                while ((n = mc_read(s_rel, rb, sizeof(rb))) > 0) {
                    char want[16];
                    int wl = sprintf(want, "R%03d", recvd);
                    if (n != wl || memcmp(rb, want, (size_t)n) != 0)
                        order_ok = 0;
                    recvd++;
                }
            }
            now += 20;
        }
        CHECK(recvd == N, "reliable: all 40 messages delivered under loss");
        CHECK(order_ok, "reliable: delivered strictly in order");
    }

    /* --- unreliable delivery (no induced loss) ---
     * One datagram per tick, drained each tick, the way a game loop uses
     * an unreliable channel. The TX ring is deliberately shallow. */
    g_drop = 0;
    {
        const int N = 10;
        int got = 0, j;
        char rb[64];
        for (j = 0; j < N; j++) {
            char m[16];
            int ml = sprintf(m, "U%03d", j), n;
            mc_write(c_unr, m, (size_t)ml);
            mc_service(s, now);
            mc_service(cl, now);
            recv_pump(&cu, cl);
            recv_pump(&su, s);
            send_pump(&cu, cl);
            send_pump(&su, s);
            while ((n = mc_read(s_unr, rb, sizeof(rb))) > 0)
                got++;
            now += 20;
        }
        /* drain any in flight */
        for (i = 0; i < 20 && got < N; i++) {
            int n;
            mc_service(s, now);
            wait_pkt(&su, 10);
            recv_pump(&su, s);
            while ((n = mc_read(s_unr, rb, sizeof(rb))) > 0)
                got++;
            now += 20;
        }
        CHECK(got == N, "unreliable: 10 datagrams delivered, 1/tick lossless");
    }

    /* --- bidirectional: server -> client reliable --- */
    g_drop = 0;
    {
        const int N = 5;
        int got = 0, j;
        char rb[64];
        for (j = 0; j < N; j++) {
            char m[16];
            int ml = sprintf(m, "S%03d", j);
            mc_write(s_rel, m, (size_t)ml);
        }
        for (i = 0; i < 50 && got < N; i++) {
            int n;
            mc_service(s, now);
            mc_service(cl, now);
            recv_pump(&cu, cl);
            recv_pump(&su, s);
            send_pump(&cu, cl);
            send_pump(&su, s);
            while ((n = mc_read(c_rel, rb, sizeof(rb))) > 0)
                got++;
            now += 20;
        }
        CHECK(got == N, "reliable: server-to-client delivery works");
    }

    mc_close(s);
    mc_close(cl);
    mc_udp_close(&su);
    mc_udp_close(&cu);

    printf(g_fail ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
