/* test_mc_udp.c : the host UDP transport over real loopback sockets */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * The protocol tests run on mc_memlink, so this is the only thing that
 * exercises mc_udp: the ephemeral bind, the address packing, the return
 * convention of a non-blocking recv, and that a datagram arrives intact
 * with the sender's address on it.
 *
 * Loopback delivery is not synchronous everywhere. Linux queues the
 * datagram inside sendto(); macOS hands it to a separate context, so a
 * non-blocking read straight after the send finds nothing. Everything here
 * that expects a datagram waits for it first.
 */

#include "mc_udp.h"
#include <stdio.h>
#include <string.h>
#include <poll.h>

static int g_fail;

#define CHECK(c, m) do { \
        if (c) printf("  ok   %s\n", m); \
        else { printf("  FAIL %s\n", m); g_fail++; } \
    } while (0)

/* Wait for a datagram, up to a generous ceiling. Returns 1 if one is
 * readable. A slow machine costs time here rather than a false failure. */
static int
wait_pkt(struct mc_udp *u, int ms)
{
    struct pollfd p;

    p.fd = u->fd;
    p.events = POLLIN;
    p.revents = 0;
    return poll(&p, 1, ms) > 0;
}

static int
same_addr(const struct mc_addr *a, const struct mc_addr *b)
{
    return a->len == b->len && memcmp(a->a, b->a, a->len) == 0;
}

int
main(void)
{
    struct mc_udp a, b;
    struct mc_addr aaddr, baddr, from, packed;
    const char msg[] = "mc_udp round trip";
    char buf[2048];             /* the transport has no MTU of its own */
    int n;

    printf("mc_udp transport test (real loopback sockets)\n");

    if (mc_udp_open(&a, "127.0.0.1", 0) != 0 ||
        mc_udp_open(&b, "127.0.0.1", 0) != 0) {
        printf("socket setup failed\n");
        return 2;
    }
    CHECK(mc_udp_local(&a, &aaddr) == 0 && mc_udp_local(&b, &baddr) == 0,
          "ephemeral bind reports a local address");
    CHECK(aaddr.len == 6 && aaddr.a[0] == 127 && aaddr.a[3] == 1,
          "local address packs as 127.0.0.1 plus a port");
    CHECK(!same_addr(&aaddr, &baddr), "two ephemeral binds differ");

    /* Nothing has been sent, so a non-blocking recv reports empty, not
     * an error. The pump loops rely on that distinction. */
    CHECK(mc_udp_recv(&a, buf, sizeof(buf), &from) == 0,
          "recv on an idle socket returns 0");

    CHECK(mc_udp_send(&a, msg, sizeof(msg), &baddr) == (int)sizeof(msg),
          "send reports the full length");
    CHECK(wait_pkt(&b, 2000), "the datagram arrives");
    n = mc_udp_recv(&b, buf, sizeof(buf), &from);
    CHECK(n == (int)sizeof(msg) && memcmp(buf, msg, sizeof(msg)) == 0,
          "the payload survives the round trip");
    CHECK(same_addr(&from, &aaddr), "the sender's address comes back");

    /* The reply proves the address handed back by recv is usable as a
     * destination, which is what connection migration depends on. */
    CHECK(mc_udp_send(&b, "ack", 3, &from) == 3, "reply to the sender");
    CHECK(wait_pkt(&a, 2000), "the reply arrives");
    n = mc_udp_recv(&a, buf, sizeof(buf), &from);
    CHECK(n == 3 && memcmp(buf, "ack", 3) == 0, "the reply is intact");
    CHECK(same_addr(&from, &baddr), "the reply carries the replier's address");

    CHECK(mc_udp_addr("192.168.1.5", 27015, &packed) == 0 &&
          packed.len == 6 && packed.a[0] == 192 && packed.a[1] == 168 &&
          packed.a[2] == 1 && packed.a[3] == 5 &&
          packed.a[4] == (27015 >> 8) && packed.a[5] == (27015 & 0xff),
          "mc_udp_addr packs a dotted quad and port");

    mc_udp_close(&a);
    mc_udp_close(&b);

    printf(g_fail ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
