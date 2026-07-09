/* game_server.c : authoritative Caves-of-Thor server over netchan/UDP */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * Pure UDP, self-contained: up to four clients, server-authoritative sim
 * at 5 Hz. Native clients connect here directly; browser clients reach the
 * same socket through the WebRTC gateway, which appears as another UDP peer.
 *
 *   game_server [port] [seed]
 */

#include "netchan.h"
#include "nc_udp.h"
#include "proto.h"
#include "game.h"
#include "game_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define MAX_PEERS  MAX_PLAYERS
#define TICK_MS    200          /* 5 Hz */
#define POLL_MS    20

static volatile sig_atomic_t running = 1;
static void on_sigint(int s) { (void)s; running = 0; }

static uint32_t
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int
udp_socket(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); exit(1); }
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

struct peer {
    struct netchan_conn *conn;
    struct netchan_chan *in_ch;    /* client's input channel (we receive) */
    struct netchan_chan *snap_ch;  /* our snapshot channel (we send) */
    struct netchan_chan *wel_ch;   /* our reliable welcome channel */
    int active;
    int player;                    /* game slot, or -1 */
    int welcomed;
};

int
main(int argc, char **argv)
{
    uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 9000;
    uint16_t seed = (argc > 2) ? (uint16_t)atoi(argv[2]) : 0x1234;

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    struct world w;
    game_init(&w, seed);

    int fd = udp_socket(port);
    printf("game server on udp/%d (seed %u), up to %d players\n",
           port, seed, MAX_PEERS);

    struct peer peers[MAX_PEERS] = {{0}};
    for (int i = 0; i < MAX_PEERS; i++) peers[i].player = -1;

    uint32_t last_tick = now_ms();

    while (running) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        poll(&pfd, 1, POLL_MS);

        if (pfd.revents & POLLIN) {
            uint8_t pkt[2048];
            struct sockaddr_storage from;
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(fd, pkt, sizeof(pkt), 0,
                                 (struct sockaddr *)&from, &fl);
            if (n > 0) {
                struct nc_addr na;
                nc_udp_from_sockaddr(&na, (struct sockaddr *)&from, fl);
                uint32_t id = netchan_peek_id(pkt, (size_t)n);

                int slot = -1;
                for (int i = 0; i < MAX_PEERS; i++)
                    if (peers[i].active && netchan_id(peers[i].conn) == id) { slot = i; break; }

                if (slot >= 0) {
                    netchan_feed(peers[slot].conn, pkt, (size_t)n, &na);
                } else {
                    for (int i = 0; i < MAX_PEERS; i++)
                        if (!peers[i].active) { slot = i; break; }
                    if (slot >= 0) {
                        peers[slot].conn = netchan_open(1);
                        netchan_feed(peers[slot].conn, pkt, (size_t)n, &na);
                        netchan_accept(peers[slot].conn);
                        peers[slot].active = 1;
                        flush(fd, peers[slot].conn);
                    }
                }
            }
        }

        /* service each peer: timers, events, input */
        for (int i = 0; i < MAX_PEERS; i++) {
            struct peer *p = &peers[i];
            if (!p->active) continue;
            netchan_service(p->conn, now_ms());

            int dead = 0;
            struct netchan_event ev;
            while (netchan_poll(p->conn, &ev)) {
                if (ev.type == NETCHAN_EV_CONNECTED) {
                    p->player = game_add_player(&w);
                    p->wel_ch = netchan_chan_open(p->conn, NETCHAN_RELIABLE,
                                                  NETCHAN_DIR_SEND, "welcome");
                    p->snap_ch = netchan_chan_open(p->conn, NETCHAN_UNRELIABLE,
                                                   NETCHAN_DIR_SEND, "snap");
                    printf("server: player %d joined (slot %d)\n", p->player, i);
                } else if (ev.type == NETCHAN_EV_CHAN_OPEN) {
                    p->in_ch = ev.ch;   /* client's PlayerInput channel */
                } else if (ev.type == NETCHAN_EV_DATA && ev.ch == p->in_ch) {
                    uint8_t buf[64];
                    int rd = netchan_chan_read(ev.ch, buf, sizeof(buf));
                    struct player_input pi;
                    if (rd > 0 && player_input_decode(&pi, buf, rd) > 0 &&
                        p->player >= 0)
                        game_set_input(&w, p->player, pi.input);
                } else if (ev.type == NETCHAN_EV_DISCONNECTED) {
                    dead = 1;
                    break;   /* stop polling: we are about to free this conn */
                }
            }
            if (dead) {
                /* Free the peer AFTER the poll loop, never during it, or the
                 * next netchan_poll would touch a closed connection. */
                printf("server: player %d left\n", p->player);
                if (p->player >= 0) {
                    w.players[p->player].alive = 0;
                    w.players[p->player].hp = 0;   /* release the slot */
                }
                netchan_close(p->conn);
                memset(p, 0, sizeof(*p));
                p->player = -1;
                continue;
            }

            /* send the welcome once the reliable channel is open */
            if (p->active && !p->welcomed && p->wel_ch &&
                netchan_chan_state(p->wel_ch) == 1 && p->player >= 0) {
                struct welcome wl = { .your_player = (uint16_t)p->player, .seed = seed };
                uint8_t buf[32];
                int m = welcome_encode(&wl, buf, sizeof(buf));
                if (m > 0 && netchan_chan_write(p->wel_ch, buf, m) == m)
                    p->welcomed = 1;
            }
        }

        /* fixed-rate simulation tick + snapshot broadcast */
        if (now_ms() - last_tick >= TICK_MS) {
            last_tick += TICK_MS;
            game_tick(&w);

            uint8_t state[GW_STATE_SIZE];
            gw_pack(&w, state, sizeof(state));
            struct snapshot sn = { .tick = w.tick, .state = state,
                                   .state_len = (uint16_t)sizeof(state) };
            uint8_t buf[512];
            int m = snapshot_encode(&sn, buf, sizeof(buf));

            for (int i = 0; i < MAX_PEERS; i++) {
                struct peer *p = &peers[i];
                if (p->active && p->snap_ch && netchan_chan_state(p->snap_ch) == 1 && m > 0)
                    netchan_chan_write(p->snap_ch, buf, m);
            }
        }

        for (int i = 0; i < MAX_PEERS; i++)
            if (peers[i].active) flush(fd, peers[i].conn);
    }

    printf("server shutting down\n");
    for (int i = 0; i < MAX_PEERS; i++)
        if (peers[i].active) netchan_close(peers[i].conn);
    close(fd);
    return 0;
}
