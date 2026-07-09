/* game_client.c : headless Caves-of-Thor client over netchan/UDP */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * A scriptable, non-interactive client used to prove the game syncs over
 * netchan-v2. It joins, reproduces the map from the join seed, drives a
 * fixed input, applies snapshots, and prints a summary. The playable
 * terminal client is built on the same message flow.
 *
 *   game_client [host] [port] [run_ms] [move_dir 0..7]
 */

#include "netchan.h"
#include "nc_udp.h"
#include "proto.h"
#include "game.h"
#include "game_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define POLL_MS 15

static uint32_t
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
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

int
main(int argc, char **argv)
{
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t port    = (argc > 2) ? (uint16_t)atoi(argv[2]) : 9000;
    uint32_t run_ms  = (argc > 3) ? (uint32_t)atoi(argv[3]) : 2500;
    uint8_t move_dir = (argc > 4) ? (uint8_t)atoi(argv[4]) : 2;   /* east */

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, host, &sa.sin_addr);
    struct nc_addr saddr;
    nc_udp_from_sockaddr(&saddr, (struct sockaddr *)&sa, sizeof(sa));

    struct netchan_conn *c = netchan_open(0);
    netchan_connect(c, &saddr);

    struct netchan_chan *in_ch = NULL, *wel_ch = NULL, *snap_ch = NULL;
    struct world w;
    memset(&w, 0, sizeof(w));
    int my_player = -1, have_map = 0, snaps = 0;
    uint32_t seq = 0, last_input = 0, start = now_ms();

    while (now_ms() - start < run_ms) {
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
                netchan_feed(c, pkt, (size_t)n, &na);
            }
        }

        netchan_service(c, now_ms());

        /* open our input channel once connected */
        if (!in_ch && netchan_state(c) == NETCHAN_STATE_CONNECTED)
            in_ch = netchan_chan_open(c, NETCHAN_UNRELIABLE, NETCHAN_DIR_SEND, "input");

        struct netchan_event ev;
        while (netchan_poll(c, &ev)) {
            if (ev.type == NETCHAN_EV_CHAN_OPEN) {
                if (netchan_chan_type(ev.ch) == NETCHAN_RELIABLE)
                    wel_ch = ev.ch;
                else
                    snap_ch = ev.ch;
            } else if (ev.type == NETCHAN_EV_DATA) {
                uint8_t buf[512];
                int rd = netchan_chan_read(ev.ch, buf, sizeof(buf));
                if (rd <= 0) continue;
                if (ev.ch == wel_ch) {
                    struct welcome wl;
                    if (welcome_decode(&wl, buf, rd) > 0) {
                        my_player = wl.your_player;
                        game_init(&w, wl.seed);   /* reproduce the static map */
                        have_map = 1;
                        printf("client: welcome, player %d, seed %u\n",
                               my_player, wl.seed);
                    }
                } else if (ev.ch == snap_ch) {
                    struct snapshot sn;
                    if (snapshot_decode(&sn, buf, rd) > 0 && have_map) {
                        gw_unpack(&w, sn.state, sn.state_len);
                        w.tick = (uint16_t)sn.tick;
                        snaps++;
                    }
                }
            }
        }

        /* drive a fixed input at ~10 Hz */
        if (in_ch && netchan_chan_state(in_ch) == 1 && now_ms() - last_input >= 100) {
            last_input = now_ms();
            struct player_input pi = { .seq = ++seq,
                                       .input = IN_MAKE(move_dir, IN_DIR_NONE) };
            uint8_t buf[32];
            int m = player_input_encode(&pi, buf, sizeof(buf));
            if (m > 0) netchan_chan_write(in_ch, buf, m);
        }

        flush(fd, c);
    }

    printf("client: state=%d, player=%d, snapshots=%d\n",
           netchan_state(c), my_player, snaps);
    if (my_player >= 0 && have_map) {
        struct player *me = &w.players[my_player];
        printf("client: my player at (%u,%u) hp=%u alive=%u, tick=%u\n",
               me->x, me->y, me->hp, me->alive, w.tick);
    }

    netchan_close(c);
    close(fd);
    return (my_player >= 0 && snaps > 0) ? 0 : 1;
}
