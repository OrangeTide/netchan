/* game_play.c : playable terminal client for the netchan-v2 game */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * Connects to a game_server over UDP and plays. Twin-stick controls:
 * arrow keys steer, WASD aims and fires in that direction, space/Z fires
 * along the way you last moved, Q quits.
 *
 *   game_play [host] [port]
 */

#include "netchan.h"
#include "nc_udp.h"
#include "proto.h"
#include "game.h"
#include "game_wire.h"
#include "render.h"
#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define MAX_DRAIN 32

static int
sgn(int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); }

/* Map a signed step to one of the 8 game directions, or -1 for none. */
static int
dir_of(int dx, int dy)
{
    dx = sgn(dx);
    dy = sgn(dy);
    if (!dx && !dy)
        return -1;
    for (int k = 0; k < 8; k++)
        if (game_dx[k] == dx && game_dy[k] == dy)
            return k;
    return -1;
}

/* Twin-stick: arrows steer, WASD aims, space/Z fires along facing. */
static uint8_t
read_input(int facing)
{
    int move = dir_of(plat_key(K_RIGHT) - plat_key(K_LEFT),
                      plat_key(K_DOWN)  - plat_key(K_UP));
    int fire = dir_of(plat_key(K_FIRE_RIGHT) - plat_key(K_FIRE_LEFT),
                      plat_key(K_FIRE_DOWN)  - plat_key(K_FIRE_UP));
    if (fire < 0 && plat_key(K_FIRE))
        fire = facing;
    return IN_MAKE(move < 0 ? IN_DIR_NONE : move,
                   fire < 0 ? IN_DIR_NONE : fire);
}

static void
message(const char *s)
{
    for (int i = 0; i < SCR_W * SCR_H; i++)
        plat_put(i % SCR_W, i / SCR_W, ' ', ATTR(C_GREY, C_BLACK));
    for (int i = 0; s[i] && i < SCR_W; i++)
        plat_put(i, SCR_H / 2, (unsigned char)s[i], ATTR(C_WHITE, C_BLACK));
    plat_present();
}

static void
frame_pace(void)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 30L * 1000000L };
    nanosleep(&ts, NULL);
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

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        fprintf(stderr, "bad host: %s\n", host);
        return 1;
    }
    struct nc_addr saddr;
    nc_udp_from_sockaddr(&saddr, (struct sockaddr *)&sa, sizeof(sa));

    struct netchan_conn *c = netchan_open(0);
    netchan_connect(c, &saddr);

    struct netchan_chan *in_ch = NULL, *wel_ch = NULL, *snap_ch = NULL;
    struct world w;
    memset(&w, 0, sizeof(w));
    int my_player = -1, have_map = 0;
    uint32_t seq = 0;

    plat_init();

    for (;;) {
        uint32_t now = plat_now_ms();
        plat_poll();
        if (plat_key(K_QUIT))
            break;

        /* drain incoming datagrams */
        uint8_t pkt[2048];
        for (int d = 0; d < MAX_DRAIN; d++) {
            struct sockaddr_storage from;
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(fd, pkt, sizeof(pkt), MSG_DONTWAIT,
                                 (struct sockaddr *)&from, &fl);
            if (n <= 0) break;
            struct nc_addr na;
            nc_udp_from_sockaddr(&na, (struct sockaddr *)&from, fl);
            netchan_feed(c, pkt, (size_t)n, &na);
        }

        netchan_service(c, now);

        if (!in_ch && netchan_state(c) == NETCHAN_STATE_CONNECTED)
            in_ch = netchan_chan_open(c, NETCHAN_UNRELIABLE,
                                      NETCHAN_DIR_SEND, "input");

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
                        game_init(&w, wl.seed);
                        have_map = 1;
                    }
                } else if (ev.ch == snap_ch) {
                    struct snapshot sn;
                    if (snapshot_decode(&sn, buf, rd) > 0 && have_map) {
                        gw_unpack(&w, sn.state, sn.state_len);
                        w.tick = (uint16_t)sn.tick;
                    }
                }
            } else if (ev.type == NETCHAN_EV_DISCONNECTED) {
                message("disconnected by server");
                goto done;
            }
        }

        /* send input every frame; the server keeps the latest */
        if (in_ch && netchan_chan_state(in_ch) == 1) {
            int facing = (my_player >= 0) ? w.players[my_player].facing : 0;
            struct player_input pi = { .seq = ++seq, .input = read_input(facing) };
            uint8_t buf[32];
            int m = player_input_encode(&pi, buf, sizeof(buf));
            if (m > 0) netchan_chan_write(in_ch, buf, m);
        }

        flush(fd, c);

        if (have_map && my_player >= 0) {
            char hud[80];
            const struct player *me = &w.players[my_player];
            snprintf(hud, sizeof(hud), "P%d  hp %2u  score %u  tick %u  [Q]uit",
                     my_player + 1, me->hp, me->score, w.tick);
            render_world(&w, my_player, hud);
        } else if (netchan_state(c) == NETCHAN_STATE_CONNECTED) {
            message("joined, loading map...");
        } else {
            message("connecting to host...");
        }

        frame_pace();
    }

done:
    plat_shutdown();
    netchan_close(c);
    close(fd);
    return 0;
}
