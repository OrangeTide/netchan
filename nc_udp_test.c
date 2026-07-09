/* nc_udp_test.c : exercise the nc_udp backend over real UDP sockets */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * Unlike netchan_test.c (which packs an nc_addr by hand and never touches
 * the network), this drives the actual nc_udp pack/unpack over two live
 * loopback sockets: address round-tripping, a full handshake, reliable
 * delivery, and connection migration onto a new client address.
 */

#include "netchan.h"
#include "nc_udp.h"
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static int tests_run, tests_passed;

#define TEST(name) do { tests_run++; printf("  %-46s ", name); fflush(stdout); } while (0)
#define PASS()     do { tests_passed++; printf("OK\n"); } while (0)
#define FAIL(msg)  do { printf("FAIL: %s\n", msg); return; } while (0)
#define CHECK(c, msg) do { if (!(c)) FAIL(msg); } while (0)

static void
test_roundtrip_v4(void)
{
    TEST("nc_udp IPv4 address round-trip");
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(0xc0a80105);   /* 192.168.1.5 */
    sin.sin_port = htons(27015);

    struct nc_addr a;
    CHECK(nc_udp_from_sockaddr(&a, (struct sockaddr *)&sin, sizeof(sin)) == 0, "pack failed");
    CHECK(a.len == 7 && a.a[0] == 4, "wrong v4 packing");

    struct sockaddr_storage ss;
    CHECK(nc_udp_to_sockaddr(&a, &ss) == sizeof(struct sockaddr_in), "unpack len");
    struct sockaddr_in *o = (struct sockaddr_in *)&ss;
    CHECK(o->sin_family == AF_INET, "family lost");
    CHECK(o->sin_addr.s_addr == sin.sin_addr.s_addr, "address lost");
    CHECK(o->sin_port == sin.sin_port, "port lost");
    PASS();
}

static void
test_roundtrip_v6(void)
{
    TEST("nc_udp IPv6 address round-trip");
    struct sockaddr_in6 sin6;
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    inet_pton(AF_INET6, "2001:db8::dead:beef", &sin6.sin6_addr);
    sin6.sin6_port = htons(9999);

    struct nc_addr a;
    CHECK(nc_udp_from_sockaddr(&a, (struct sockaddr *)&sin6, sizeof(sin6)) == 0, "pack failed");
    CHECK(a.len == 19 && a.a[0] == 6, "wrong v6 packing");

    struct sockaddr_storage ss;
    CHECK(nc_udp_to_sockaddr(&a, &ss) == sizeof(struct sockaddr_in6), "unpack len");
    struct sockaddr_in6 *o = (struct sockaddr_in6 *)&ss;
    CHECK(memcmp(&o->sin6_addr, &sin6.sin6_addr, 16) == 0, "address lost");
    CHECK(o->sin6_port == sin6.sin6_port, "port lost");
    PASS();
}

static int
bind_lo(struct sockaddr_in *out)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;                            /* ephemeral */
    bind(fd, (struct sockaddr *)&a, sizeof(a));
    socklen_t sl = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &sl);
    *out = a;
    return fd;
}

static void
flush(int fd, struct netchan_conn *c)
{
    uint8_t buf[2048];
    struct nc_addr dst = {0};
    for (;;) {
        size_t n = netchan_send_next(c, buf, sizeof(buf), &dst);
        if (n == 0) break;
        struct sockaddr_storage ss;
        socklen_t sl = nc_udp_to_sockaddr(&dst, &ss);
        if (sl == 0) continue;
        sendto(fd, buf, n, 0, (struct sockaddr *)&ss, sl);
    }
}

static int
drain(int fd, struct netchan_conn *c)
{
    uint8_t pkt[2048];
    struct sockaddr_storage from;
    socklen_t fl = sizeof(from);
    int got = 0;
    for (;;) {
        ssize_t n = recvfrom(fd, pkt, sizeof(pkt), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &fl);
        if (n <= 0) break;
        struct nc_addr a;
        nc_udp_from_sockaddr(&a, (struct sockaddr *)&from, fl);
        netchan_feed(c, pkt, (size_t)n, &a);
        got++;
        fl = sizeof(from);
    }
    return got;
}

static void
pump_both(int cfd, struct netchan_conn *cl, int sfd, struct netchan_conn *sv)
{
    for (int i = 0; i < 16; i++) {
        flush(cfd, cl);
        flush(sfd, sv);
        int a = drain(sfd, sv);
        int b = drain(cfd, cl);
        if (a == 0 && b == 0 && i > 2) break;
    }
}

static int
read_chan_msg(struct netchan_conn *c, const char *want, size_t wlen)
{
    struct netchan_event ev;
    struct netchan_chan *ch = NULL;
    while (netchan_poll(c, &ev))
        if (ev.type == NETCHAN_EV_DATA && ev.ch) ch = ev.ch;
    if (!ch) return 0;
    char buf[256];
    int rd = netchan_chan_read(ch, buf, sizeof(buf));
    return rd == (int)wlen && memcmp(buf, want, wlen) == 0;
}

static void
test_socket_session_and_migration(void)
{
    TEST("real-socket session + migration");

    struct sockaddr_in caddr, saddr;
    int cfd = bind_lo(&caddr);
    int sfd = bind_lo(&saddr);

    struct nc_addr snaddr;
    nc_udp_from_sockaddr(&snaddr, (struct sockaddr *)&saddr, sizeof(saddr));

    struct netchan_conn *cl = netchan_open(0);
    struct netchan_conn *sv = netchan_open(1);

    netchan_connect(cl, &snaddr);
    flush(cfd, cl);
    /* server accepts on its first received packet */
    for (int i = 0; i < 16; i++) {
        if (drain(sfd, sv) && netchan_state(sv) == NETCHAN_STATE_CONNECTING)
            netchan_accept(sv);
        pump_both(cfd, cl, sfd, sv);
        if (netchan_state(cl) == NETCHAN_STATE_CONNECTED &&
            netchan_state(sv) == NETCHAN_STATE_CONNECTED)
            break;
    }
    CHECK(netchan_state(cl) == NETCHAN_STATE_CONNECTED, "client not connected");
    CHECK(netchan_state(sv) == NETCHAN_STATE_CONNECTED, "server not connected");

    struct netchan_event ev;
    while (netchan_poll(cl, &ev)) {}
    while (netchan_poll(sv, &ev)) {}

    /* reliable channel + baseline delivery */
    struct netchan_chan *ch = netchan_chan_open(cl, NETCHAN_RELIABLE,
                                                NETCHAN_DIR_SEND, "state");
    CHECK(ch != NULL, "chan_open failed");
    pump_both(cfd, cl, sfd, sv);

    const char *m1 = "over a real socket";
    CHECK(netchan_chan_write(ch, m1, strlen(m1)) == (int)strlen(m1), "write m1");
    pump_both(cfd, cl, sfd, sv);
    CHECK(read_chan_msg(sv, m1, strlen(m1)), "server did not receive m1");

    /* migration: client abandons its socket and rebinds to a new address */
    close(cfd);
    struct sockaddr_in caddr2;
    int cfd2 = bind_lo(&caddr2);
    CHECK(caddr2.sin_port != caddr.sin_port, "expected a new source port");

    const char *m2 = "after moving to a new address";
    CHECK(netchan_chan_write(ch, m2, strlen(m2)) == (int)strlen(m2), "write m2");
    pump_both(cfd2, cl, sfd, sv);
    CHECK(read_chan_msg(sv, m2, strlen(m2)), "server did not receive m2 after migration");

    /* the reliable m2 is only acked if the server migrated its replies to
     * the new socket, so a completed ack proves the migration end to end. */
    struct netchan_chan_stats st;
    netchan_chan_stats(ch, &st);
    CHECK(st.msgs_acked >= 2, "server ack did not follow the migrated address");

    netchan_close(cl);
    netchan_close(sv);
    close(cfd2);
    close(sfd);
    PASS();
}

int
main(void)
{
    printf("nc_udp tests:\n");
    test_roundtrip_v4();
    test_roundtrip_v6();
    test_socket_session_and_migration();
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
