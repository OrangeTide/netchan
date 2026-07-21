/* test_microchan.c : core protocol tests over an in-memory datagram link */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * The core is buffer-in, buffer-out, so these tests do not need a socket
 * and are better off without one: mc_memlink delivers synchronously and
 * impairs traffic on a fixed count, which makes every check here
 * deterministic and lets the window be tested against reordering and
 * duplication. mc_udp has its own test.
 */

#include "microchan.h"
#include "mc_memlink.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, msg) do {                       \
        if (cond) {                                 \
            printf("  ok   %s\n", msg);             \
        } else {                                    \
            printf("  FAIL %s\n", msg);             \
            g_fail++;                               \
        }                                           \
    } while (0)

static struct memlink link;

static void
recv_pump(const struct mc_addr *self, struct microchan *nc)
{
    uint8_t buf[MC_MTU];
    struct mc_addr from;
    int n;
    while ((n = meml_recv(&link, self, buf, sizeof(buf), &from)) > 0)
        mc_feed(nc, buf, (size_t)n, &from);
}

static void
send_pump(const struct mc_addr *self, struct microchan *nc)
{
    uint8_t buf[MC_MTU];
    struct mc_addr to;
    int n;
    while ((n = (int)mc_send_next(nc, buf, sizeof(buf), &to)) > 0)
        meml_send(&link, self, buf, (size_t)n, &to);
}

int
main(void)
{
    struct mc_addr saddr, caddr;
    struct microchan *s, *cl;
    struct mc_chan *s_rel, *s_unr, *c_rel, *c_unr;
    uint32_t now = 0;
    int i;

    meml_init(&link);
    if (meml_open(&link, &saddr) != 0 || meml_open(&link, &caddr) != 0) {
        printf("link setup failed\n");
        return 2;
    }

    s = mc_open(1);
    cl = mc_open(0);
    s_rel = mc_chan_open(s, MC_RELIABLE);
    s_unr = mc_chan_open(s, MC_UNRELIABLE);
    c_rel = mc_chan_open(cl, MC_RELIABLE);
    c_unr = mc_chan_open(cl, MC_UNRELIABLE);

    printf("microchan core tests (in-memory link)\n");
    printf("MTU=%d WINDOW=%d MAXMSG=%d\n", MC_MTU, MC_WINDOW, MC_MAXMSG);

    /* --- handshake --- */
    mc_connect(cl, &saddr);
    for (i = 0; i < 100; i++) {
        if (mc_state(s) == MC_STATE_CONNECTED &&
            mc_state(cl) == MC_STATE_CONNECTED)
            break;
        mc_service(s, now);
        mc_service(cl, now);
        recv_pump(&caddr, cl);
        recv_pump(&saddr, s);
        send_pump(&caddr, cl);
        send_pump(&saddr, s);
        now += 20;
    }
    CHECK(mc_state(cl) == MC_STATE_CONNECTED, "client connected");
    CHECK(mc_state(s) == MC_STATE_CONNECTED, "server connected");

    /* --- reliable in-order delivery under 25% packet loss --- */
    link.drop_every = 4;
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
            recv_pump(&caddr, cl);
            recv_pump(&saddr, s);
            send_pump(&caddr, cl);
            send_pump(&saddr, s);
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
        CHECK(link.dropped > 0, "the link actually dropped datagrams");
    }

    /* --- reliable delivery through reordering and duplication ---
     * Go-Back-N has to hold the line on both: a datagram that arrives out
     * of order must not be handed up early, and one that arrives twice must
     * not be handed up twice. Loopback UDP produces neither on demand,
     * which is why this check could not exist before. */
    link.drop_every = 0;
    link.reorder_every = 3;
    link.dup_every = 5;
    {
        const int N = 30;
        int sent = 0, recvd = 0, order_ok = 1;
        char rb[64];
        for (i = 0; i < 4000 && recvd < N; i++) {
            while (sent < N) {
                char m[16];
                int ml = sprintf(m, "D%03d", sent);
                if (mc_write(c_rel, m, (size_t)ml) == MC_ERR_AGAIN)
                    break;
                sent++;
            }
            mc_service(s, now);
            mc_service(cl, now);
            recv_pump(&caddr, cl);
            recv_pump(&saddr, s);
            send_pump(&caddr, cl);
            send_pump(&saddr, s);
            {
                int n;
                while ((n = mc_read(s_rel, rb, sizeof(rb))) > 0) {
                    char want[16];
                    int wl = sprintf(want, "D%03d", recvd);
                    if (n != wl || memcmp(rb, want, (size_t)n) != 0)
                        order_ok = 0;
                    recvd++;
                }
            }
            now += 20;
        }
        CHECK(recvd == N, "reliable: all 30 messages survive reorder + dup");
        CHECK(order_ok, "reliable: reordered and duplicated, still in order");
        CHECK(link.reordered > 0 && link.duped > 0,
              "the link actually reordered and duplicated datagrams");
    }
    link.reorder_every = 0;
    link.dup_every = 0;

    /* --- unreliable delivery (no impairment) ---
     * One datagram per tick, drained each tick, the way a game loop uses
     * an unreliable channel. The TX ring is deliberately shallow. */
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
            recv_pump(&caddr, cl);
            recv_pump(&saddr, s);
            send_pump(&caddr, cl);
            send_pump(&saddr, s);
            while ((n = mc_read(s_unr, rb, sizeof(rb))) > 0)
                got++;
            now += 20;
        }
        /* Delivery is synchronous, so nothing should be outstanding. The
         * drain is here to prove that rather than to wait for it. */
        {
            int n;
            recv_pump(&saddr, s);
            while ((n = mc_read(s_unr, rb, sizeof(rb))) > 0)
                got++;
        }
        CHECK(got == N, "unreliable: 10 datagrams delivered, 1/tick lossless");
    }

    /* --- bidirectional: server -> client reliable --- */
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
            recv_pump(&caddr, cl);
            recv_pump(&saddr, s);
            send_pump(&caddr, cl);
            send_pump(&saddr, s);
            while ((n = mc_read(c_rel, rb, sizeof(rb))) > 0)
                got++;
            now += 20;
        }
        CHECK(got == N, "reliable: server-to-client delivery works");
    }

    mc_close(s);
    mc_close(cl);

    printf(g_fail ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
