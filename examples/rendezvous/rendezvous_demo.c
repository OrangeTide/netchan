/*
 * rendezvous_demo.c : the whole reachability path in one program
 *
 * Two peers that have never heard of each other end up with a netchan session
 * between them, and the three sibling layers each do their part on the way:
 *
 *   nat    nc_stun_*     each peer asks the rendezvous "what address did my
 *                        packet come from?" and learns its own public address.
 *          nc_roomcode_* the host writes its address as a short code a person
 *                        could read aloud, and the guest reads it back.
 *   reach  nc_reach_*    both peers derive the same session and punch at each
 *                        other at once, opening the path between them.
 *   core   netchan_*     the ordinary handshake runs over the opened path and
 *                        a message crosses in each direction.
 *
 * It runs as one process so it is deterministic and needs nothing outside it: a
 * rendezvous socket and two peer sockets on the loopback, pumped by one loop.
 * On the loopback there is no NAT to traverse, so the punch always succeeds at
 * once; on a real network it is the step that opens the two routers. Every
 * packet on the wire is the real thing a separate process would send.
 *
 * The rendezvous here answers STUN and pairs two peers by a shared code. A real
 * deployment uses a public STUN server and whatever introduction channel the
 * application already trusts; the CONNECT_REDIRECT frame is the shape that
 * answer takes. The STUN responder below is a demo stand-in: the library builds
 * and parses STUN on the client side only, because a STUN server is not netchan.
 */

#include "netchan.h"
#include "nc_udp.h"
#include "nc_nat.h"
#include "nc_reach.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define STUN_COOKIE   0x2112a442u
#define RV_MAGIC_0 'R'
#define RV_MAGIC_1 'V'
#define RV_MAGIC_2 '0'
#define RV_MAGIC_3 '1'
#define RV_REGISTER  1
#define RV_INTRODUCE 2

#define STUN_EVERY_MS   200
#define REG_EVERY_MS    200
#define PUNCH_EVERY_MS  150
#define DEADLINE_MS   10000

static uint32_t
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* A non-blocking UDP socket bound to the loopback on an ephemeral port, with
 * its bound address read back into *self as an nc_addr. */
static int
bind_loopback(struct nc_addr *self)
{
    struct sockaddr_in a;
    socklen_t sl = sizeof(a);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) { perror("socket"); exit(1); }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); exit(1); }
    if (getsockname(fd, (struct sockaddr *)&a, &sl) < 0) { perror("getsockname"); exit(1); }
    nc_udp_from_sockaddr(self, (struct sockaddr *)&a, sl);
    return fd;
}

static void
send_to(int fd, const struct nc_addr *dst, const void *buf, size_t n)
{
    struct sockaddr_storage ss;
    socklen_t sl = nc_udp_to_sockaddr(dst, &ss);

    if (sl == 0) return;
    sendto(fd, buf, n, 0, (struct sockaddr *)&ss, sl);
}

/* One datagram, or -1 when the socket is empty. The source is returned as an
 * nc_addr so a caller can both read it and send a reply back to it. */
static long
recv_from(int fd, void *buf, size_t cap, struct nc_addr *src)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    ssize_t n = recvfrom(fd, buf, cap, MSG_DONTWAIT, (struct sockaddr *)&ss, &sl);

    if (n <= 0) return -1;
    nc_udp_from_sockaddr(src, (struct sockaddr *)&ss, sl);
    return (long)n;
}

static int
is_stun_request(const uint8_t *p, size_t n)
{
    uint32_t cookie;

    if (n < NC_STUN_REQUEST_LEN || p[0] != 0x00 || p[1] != 0x01)
        return 0;
    cookie = (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16
           | (uint32_t)p[6] << 8 | p[7];
    return cookie == STUN_COOKIE;
}

static int
is_rv_message(const uint8_t *p, size_t n)
{
    return n >= 5 && p[0] == RV_MAGIC_0 && p[1] == RV_MAGIC_1
        && p[2] == RV_MAGIC_2 && p[3] == RV_MAGIC_3;
}

/*----------------------------------------------------------------------------
 * The rendezvous: a STUN responder and a two-peer matchmaker.
 *--------------------------------------------------------------------------*/

static void
put_be16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

/*
 * A STUN binding success for the request in req, naming where it came from. The
 * address goes in XOR-MAPPED-ADDRESS, obscured under the cookie, which is what
 * the library's parser prefers. This is the one piece a real STUN server would
 * do instead; the library does not, on purpose.
 */
static int
stun_success(uint8_t *out, const uint8_t *req, const struct nc_addr *src)
{
    uint32_t ip;
    uint16_t port;

    if (!nc_addr_get_ipv4(src, &ip, &port))
        return 0;
    ip ^= STUN_COOKIE;
    port ^= (uint16_t)(STUN_COOKIE >> 16);

    put_be16(out + 0, 0x0101);              /* binding success */
    put_be16(out + 2, 12);                  /* one 8-byte attribute */
    memcpy(out + 4, req + 4, 4);            /* the cookie, echoed */
    memcpy(out + 8, req + 8, NC_STUN_TXN_LEN); /* the caller's id, echoed */
    put_be16(out + 20, 0x0020);             /* XOR-MAPPED-ADDRESS */
    put_be16(out + 22, 8);
    out[24] = 0;
    out[25] = 0x01;                         /* IPv4 */
    put_be16(out + 26, port);
    out[28] = (uint8_t)(ip >> 24);
    out[29] = (uint8_t)(ip >> 16);
    out[30] = (uint8_t)(ip >> 8);
    out[31] = (uint8_t)ip;
    return 32;
}

struct rv_entry {
    char code[NC_ROOMCODE_BUFSIZE];
    struct nc_addr addr;
    int used;
    int introduced;
};

struct rv {
    int fd;
    struct nc_addr addr;
    struct rv_entry room[4];
};

/* Send each peer in a full room the other's address, once. */
static void
rv_match(struct rv *r)
{
    int i, j;

    for (i = 0; i < 4; i++) {
        if (!r->room[i].used || r->room[i].introduced)
            continue;
        for (j = 0; j < 4; j++) {
            uint8_t msg[32];
            const struct nc_addr *peer;

            if (j == i || !r->room[j].used)
                continue;
            if (strcmp(r->room[i].code, r->room[j].code) != 0)
                continue;

            /* i and j share a code: introduce each to the other. */
            peer = &r->room[j].addr;
            msg[0] = RV_MAGIC_0; msg[1] = RV_MAGIC_1;
            msg[2] = RV_MAGIC_2; msg[3] = RV_MAGIC_3;
            msg[4] = RV_INTRODUCE;
            msg[5] = peer->len;
            memcpy(msg + 6, peer->a, peer->len);
            send_to(r->fd, &r->room[i].addr, msg, (size_t)(6 + peer->len));
            r->room[i].introduced = 1;

            peer = &r->room[i].addr;
            msg[5] = peer->len;
            memcpy(msg + 6, peer->a, peer->len);
            send_to(r->fd, &r->room[j].addr, msg, (size_t)(6 + peer->len));
            r->room[j].introduced = 1;
            break;
        }
    }
}

static void
rv_register(struct rv *r, const char *code, const struct nc_addr *from)
{
    int i, free_slot = -1;

    for (i = 0; i < 4; i++) {
        if (r->room[i].used && strcmp(r->room[i].code, code) == 0 &&
            r->room[i].addr.len == from->len &&
            memcmp(r->room[i].addr.a, from->a, from->len) == 0)
            return;                         /* already registered */
        if (!r->room[i].used && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        return;
    snprintf(r->room[free_slot].code, sizeof(r->room[free_slot].code), "%s", code);
    r->room[free_slot].addr = *from;
    r->room[free_slot].used = 1;
    r->room[free_slot].introduced = 0;
}

static void
rv_tick(struct rv *r)
{
    uint8_t buf[256];
    struct nc_addr from;
    long n;

    while ((n = recv_from(r->fd, buf, sizeof(buf), &from)) >= 0) {
        if (is_stun_request(buf, (size_t)n)) {
            uint8_t out[64];
            int m = stun_success(out, buf, &from);
            if (m > 0)
                send_to(r->fd, &from, out, (size_t)m);
        } else if (is_rv_message(buf, (size_t)n) && buf[4] == RV_REGISTER) {
            char code[NC_ROOMCODE_BUFSIZE];
            size_t clen = (size_t)n - 5;
            if (clen >= sizeof(code)) clen = sizeof(code) - 1;
            memcpy(code, buf + 5, clen);
            code[clen] = '\0';
            rv_register(r, code, &from);
        }
    }
    rv_match(r);
}

/*----------------------------------------------------------------------------
 * A peer: learn, be introduced, punch, connect, exchange a message.
 *--------------------------------------------------------------------------*/

enum { ST_STUN, ST_REGISTER, ST_PUNCH, ST_NET, ST_DONE };

struct peer {
    const char *name;
    int fd;
    struct nc_addr self;        /* our public address, from STUN */
    struct nc_addr rv;          /* the rendezvous */
    int is_host;

    uint8_t txn[NC_STUN_TXN_LEN];
    char code[NC_ROOMCODE_BUFSIZE];
    int have_code;

    struct nc_addr partner;     /* the address the rendezvous introduced */
    int have_partner;

    struct nc_reach reach;
    int have_session;
    struct nc_addr peer_real;   /* where the partner's punch actually came from */
    int path_open;

    struct netchan_conn *conn;
    struct netchan_chan *send_ch;
    int accepted;
    int got_msg;

    int state;
    uint32_t last_stun, last_reg, last_punch;
};

/* The shared code the host hands the guest out of band. */
static char g_code[NC_ROOMCODE_BUFSIZE];
static int  g_code_ready;

static void
peer_init(struct peer *p, const char *name, const struct nc_addr *rv, int is_host)
{
    uint32_t seed = now_ms() ^ (uint32_t)(uintptr_t)p;
    int i;

    memset(p, 0, sizeof(*p));
    p->name = name;
    p->rv = *rv;
    p->is_host = is_host;
    p->fd = bind_loopback(&p->self);
    p->state = ST_STUN;

    /* A real caller draws the transaction id from a random source, which is the
     * whole anti-spoofing check. This demo seeds a small generator from the
     * clock and the socket so the two peers differ; it is not a model to copy. */
    for (i = 0; i < NC_STUN_TXN_LEN; i++) {
        seed = seed * 1664525u + 1013904223u;
        p->txn[i] = (uint8_t)(seed >> 24);
    }
}

static void
net_flush(struct peer *p)
{
    uint8_t buf[2048];
    struct nc_addr dst;

    if (!p->conn)
        return;
    for (;;) {
        size_t n = netchan_send_next(p->conn, buf, sizeof(buf), &dst, now_ms());
        if (n == 0) break;
        send_to(p->fd, &dst, buf, n);
    }
}

/* Drain the socket, routing each datagram to the layer it belongs to. */
static void
peer_drain(struct peer *p)
{
    uint8_t buf[2048];
    struct nc_addr from;
    long n;

    while ((n = recv_from(p->fd, buf, sizeof(buf), &from)) >= 0) {
        if (n >= NC_STUN_REQUEST_LEN && buf[0] == 0x01 && buf[1] == 0x01) {
            /* a STUN success from the rendezvous: our own public address */
            if (p->state == ST_STUN &&
                nc_stun_parse_response(buf, (size_t)n, p->txn, &p->self)
                    == NC_NAT_OK) {
                printf("%s: STUN says my address is ", p->name);
                {
                    uint32_t ip; uint16_t port;
                    nc_addr_get_ipv4(&p->self, &ip, &port);
                    printf("%u.%u.%u.%u:%u\n", (ip >> 24) & 0xff,
                           (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff, port);
                }
                p->state = ST_REGISTER;
            }
            continue;
        }
        if (is_rv_message(buf, (size_t)n) && buf[4] == RV_INTRODUCE) {
            uint8_t len = buf[5];
            if (!p->have_partner && len <= NC_ADDR_MAX &&
                (size_t)n >= (size_t)(6 + len)) {
                memset(&p->partner, 0, sizeof(p->partner));
                p->partner.len = len;
                memcpy(p->partner.a, buf + 6, len);
                p->have_partner = 1;
                printf("%s: rendezvous introduced my partner\n", p->name);
            }
            continue;
        }
        if (p->have_session) {
            int kind = nc_reach_parse(&p->reach, buf, (size_t)n);
            if (kind == NC_REACH_PUNCH) {
                /* the partner reached us; learn where from and ack it */
                p->peer_real = from;
                {
                    uint8_t ack[NC_REACH_PACKET_LEN];
                    nc_reach_build(ack, sizeof(ack), &p->reach, NC_REACH_PUNCH_ACK);
                    send_to(p->fd, &from, ack, sizeof(ack));
                }
                continue;
            }
            if (kind == NC_REACH_PUNCH_ACK) {
                /* our punch reached the partner: the round trip is proven */
                p->peer_real = from;
                if (!p->path_open) {
                    p->path_open = 1;
                    printf("%s: path open to the partner\n", p->name);
                }
                continue;
            }
        }
        if (p->conn) {
            netchan_feed(p->conn, buf, (size_t)n, &from, now_ms());
            /* The netchan server is whichever peer the session made the server,
             * not the peer that hosted the room. It accepts the first INIT. */
            if (p->reach.role == NC_REACH_ROLE_SERVER && !p->accepted &&
                netchan_state(p->conn) == NETCHAN_STATE_CONNECTING) {
                netchan_accept(p->conn);
                p->accepted = 1;
            }
        }
    }
}

static void
peer_events(struct peer *p)
{
    struct netchan_event ev;

    while (netchan_poll(p->conn, &ev)) {
        switch (ev.type) {
        case NETCHAN_EV_CONNECTED: {
            char hello[64];
            uint32_t ip; uint16_t port;
            nc_addr_get_ipv4(&p->self, &ip, &port);
            p->send_ch = netchan_chan_open(p->conn, NETCHAN_RELIABLE,
                                           NETCHAN_DIR_SEND, "hello");
            snprintf(hello, sizeof(hello), "hello from %s at :%u", p->name, port);
            netchan_chan_write(p->send_ch, hello, strlen(hello));
            break;
        }
        case NETCHAN_EV_DATA: {
            char buf[128];
            int rd = netchan_chan_read(ev.ch, buf, sizeof(buf) - 1);
            if (rd > 0) {
                buf[rd] = '\0';
                printf("%s: GOT \"%s\"\n", p->name, buf);
                p->got_msg = 1;
            }
            break;
        }
        default:
            break;
        }
    }
}

static void
peer_tick(struct peer *p)
{
    uint32_t t = now_ms();

    peer_drain(p);

    switch (p->state) {
    case ST_STUN:
        if (t - p->last_stun >= STUN_EVERY_MS) {
            uint8_t req[NC_STUN_REQUEST_LEN];
            nc_stun_build_request(req, sizeof(req), p->txn);
            send_to(p->fd, &p->rv, req, sizeof(req));
            p->last_stun = t;
        }
        break;

    case ST_REGISTER:
        /* The host turns its own address into a room code and publishes it; the
         * guest takes that code. Both register under it so the rendezvous can
         * pair them. The host also reads the code straight back to show the
         * codec round-trips, and the guest decodes it to recover the host's
         * address as a cross-check once the introduction arrives. */
        if (!p->have_code) {
            if (p->is_host) {
                struct nc_addr check;
                nc_roomcode_encode(p->code, sizeof(p->code), &p->self);
                p->have_code = 1;
                if (nc_roomcode_decode(p->code, &check) == NC_NAT_OK)
                    printf("%s: room code %s (round-trips)\n", p->name, p->code);
                snprintf(g_code, sizeof(g_code), "%s", p->code);
                g_code_ready = 1;
            } else if (g_code_ready) {
                struct nc_addr host_addr;
                snprintf(p->code, sizeof(p->code), "%s", g_code);
                p->have_code = 1;
                if (nc_roomcode_decode(p->code, &host_addr) == NC_NAT_OK) {
                    uint32_t ip; uint16_t port;
                    nc_addr_get_ipv4(&host_addr, &ip, &port);
                    printf("%s: decoded room code to %u.%u.%u.%u:%u\n", p->name,
                           (ip >> 24) & 0xff, (ip >> 16) & 0xff,
                           (ip >> 8) & 0xff, ip & 0xff, port);
                }
            }
        }
        if (p->have_code && t - p->last_reg >= REG_EVERY_MS) {
            uint8_t msg[NC_ROOMCODE_BUFSIZE + 5];
            size_t clen = strlen(p->code);
            msg[0] = RV_MAGIC_0; msg[1] = RV_MAGIC_1;
            msg[2] = RV_MAGIC_2; msg[3] = RV_MAGIC_3;
            msg[4] = RV_REGISTER;
            memcpy(msg + 5, p->code, clen);
            send_to(p->fd, &p->rv, msg, 5 + clen);
            p->last_reg = t;
        }
        if (p->have_partner) {
            if (nc_reach_session(&p->reach, &p->self, &p->partner) == NC_REACH_OK) {
                p->have_session = 1;
                printf("%s: session ready, I am the %s\n", p->name,
                       p->reach.role == NC_REACH_ROLE_CLIENT ? "client" : "server");
                p->state = ST_PUNCH;
            }
        }
        break;

    case ST_PUNCH:
        if (t - p->last_punch >= PUNCH_EVERY_MS) {
            uint8_t punch[NC_REACH_PACKET_LEN];
            nc_reach_build(punch, sizeof(punch), &p->reach, NC_REACH_PUNCH);
            send_to(p->fd, &p->partner, punch, sizeof(punch));
            p->last_punch = t;
        }
        if (p->path_open) {
            /* The opened path is validated address and all: the client dials it
             * and the server listens, with the role the session already fixed. */
            if (p->reach.role == NC_REACH_ROLE_CLIENT) {
                p->conn = netchan_open(0);
                netchan_connect(p->conn, &p->peer_real);
            } else {
                p->conn = netchan_open(1);
            }
            p->state = ST_NET;
        }
        break;

    case ST_NET:
        netchan_service(p->conn, now_ms());
        peer_events(p);
        net_flush(p);
        if (p->got_msg)
            p->state = ST_DONE;
        break;

    case ST_DONE:
    default:
        break;
    }
}

int
main(void)
{
    struct rv rv;
    struct peer host, guest;
    uint32_t start = now_ms();
    int ok;

    memset(&rv, 0, sizeof(rv));
    rv.fd = bind_loopback(&rv.addr);

    peer_init(&host, "host", &rv.addr, 1);
    peer_init(&guest, "guest", &rv.addr, 0);

    printf("rendezvous up; host and guest have never met\n");

    while (!(host.state == ST_DONE && guest.state == ST_DONE) &&
           now_ms() - start < DEADLINE_MS) {
        struct pollfd pfd[3];

        pfd[0].fd = rv.fd;    pfd[0].events = POLLIN;
        pfd[1].fd = host.fd;  pfd[1].events = POLLIN;
        pfd[2].fd = guest.fd; pfd[2].events = POLLIN;
        poll(pfd, 3, 20);

        rv_tick(&rv);
        peer_tick(&host);
        peer_tick(&guest);
    }

    ok = host.got_msg && guest.got_msg;
    printf("%s: both peers exchanged a message over the punched path\n",
           ok ? "OK" : "FAIL");

    if (host.conn) { netchan_disconnect(host.conn); net_flush(&host); netchan_close(host.conn); }
    if (guest.conn) { netchan_disconnect(guest.conn); net_flush(&guest); netchan_close(guest.conn); }
    close(host.fd);
    close(guest.fd);
    close(rv.fd);
    return ok ? 0 : 1;
}
