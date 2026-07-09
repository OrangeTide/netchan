/* web_client.c : in-browser Caves-of-Thor client over netchan/WebSocket */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * The remote counterpart to web_demo.c. Where that build ran the server and
 * client together over a loopback to prove the stack, this one is client only:
 * it talks to a real, remote game server through the WebSocket gateway. The
 * game and the netchan core are byte-for-byte the same as the native client;
 * the only thing that changed is the transport file, nc_web.c, exactly the
 * outcome the nc_addr seam was built for.
 *
 * The driver stays in JavaScript (page requestAnimationFrame, or a headless
 * test harness): main() connects, then each web_step() advances one frame.
 * Inbound datagrams arrive out of band, whenever the WebSocket delivers a
 * message, via web_client_recv().
 */

#include "netchan.h"
#include "nc_web.h"
#include "proto.h"
#include "game.h"
#include "game_wire.h"
#include "render.h"
#include "plat.h"

#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static struct netchan_conn *cl;
static struct netchan_chan  *cl_in, *cl_wel, *cl_snap;
static struct world cw;                    /* the client's rendered view */
static int      my_player = -1, have_map = 0, snaps = 0;
static uint32_t seq, last_input;

/* Small status accessors so a driver (the page, or a headless test) can tell
 * whether the client joined and is receiving snapshots. */
EMSCRIPTEN_KEEPALIVE int web_client_player(void) { return my_player; }
EMSCRIPTEN_KEEPALIVE int web_client_snaps(void)  { return snaps; }

/* Push everything netchan wants to send out over the WebSocket. */
static void
flush(void)
{
    uint8_t buf[2048];
    struct nc_addr dst;
    for (;;) {
        size_t n = netchan_send_next(cl, buf, sizeof(buf), &dst);
        if (n == 0)
            break;
        nc_web_send(buf, n);
    }
}

static uint8_t
read_input(int facing)
{
    static const int8_t dxy[8][2] = {
        {0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1},
    };
    int mx = plat_key(K_RIGHT) - plat_key(K_LEFT);
    int my = plat_key(K_DOWN)  - plat_key(K_UP);
    int fx = plat_key(K_FIRE_RIGHT) - plat_key(K_FIRE_LEFT);
    int fy = plat_key(K_FIRE_DOWN)  - plat_key(K_FIRE_UP);
    int move = IN_DIR_NONE, fire = IN_DIR_NONE;
    for (int k = 0; k < 8; k++) {
        if (dxy[k][0] == (mx>0)-(mx<0) && dxy[k][1] == (my>0)-(my<0) && (mx||my))
            move = k;
        if (dxy[k][0] == (fx>0)-(fx<0) && dxy[k][1] == (fy>0)-(fy<0) && (fx||fy))
            fire = k;
    }
    if (fire == IN_DIR_NONE && plat_key(K_FIRE))
        fire = facing;
    return IN_MAKE(move, fire);
}

/* JavaScript calls this when the WebSocket delivers a binary message: the
 * bytes are already sitting in nc_web_inbuf(). */
EMSCRIPTEN_KEEPALIVE void
web_client_recv(int len)
{
    if (len > 0 && (size_t)len <= NC_WEB_INBUF)
        netchan_feed(cl, nc_web_inbuf(), (size_t)len, &NC_WEB_PEER);
}

EMSCRIPTEN_KEEPALIVE void
web_step(void)
{
    uint32_t now = plat_now_ms();
    plat_poll();
    netchan_service(cl, now);

    if (!cl_in && netchan_state(cl) == NETCHAN_STATE_CONNECTED)
        cl_in = netchan_chan_open(cl, NETCHAN_UNRELIABLE, NETCHAN_DIR_SEND, "input");

    struct netchan_event ev;
    while (netchan_poll(cl, &ev)) {
        if (ev.type == NETCHAN_EV_CHAN_OPEN) {
            if (netchan_chan_type(ev.ch) == NETCHAN_RELIABLE) cl_wel = ev.ch;
            else cl_snap = ev.ch;
        } else if (ev.type == NETCHAN_EV_DATA) {
            uint8_t b[512];
            int rd = netchan_chan_read(ev.ch, b, sizeof(b));
            if (rd <= 0) continue;
            if (ev.ch == cl_wel) {
                struct welcome wl;
                if (welcome_decode(&wl, b, rd) > 0) {
                    my_player = wl.your_player;
                    game_init(&cw, wl.seed);
                    have_map = 1;
                }
            } else if (ev.ch == cl_snap) {
                struct snapshot sn;
                if (snapshot_decode(&sn, b, rd) > 0 && have_map) {
                    gw_unpack(&cw, sn.state, sn.state_len);
                    cw.tick = (uint16_t)sn.tick;
                    snaps++;
                }
            }
        }
    }

    if (cl_in && netchan_chan_state(cl_in) == 1 && now - last_input >= 66) {
        last_input = now;
        int facing = (have_map && my_player >= 0) ? cw.players[my_player].facing : 0;
        struct player_input pi = { .seq = ++seq, .input = read_input(facing) };
        uint8_t b[32];
        int m = player_input_encode(&pi, b, sizeof(b));
        if (m > 0) netchan_chan_write(cl_in, b, m);
    }

    if (have_map && my_player >= 0) {
        char hud[80];
        const struct player *me = &cw.players[my_player];
        snprintf(hud, sizeof(hud), "hp %2u  score %u  tick %u  arrows move, WASD fire",
                 me->hp, me->score, cw.tick);
        render_world(&cw, my_player, hud);
    }

    flush();
}

int
main(void)
{
    plat_init();
    cl = netchan_open(0);
    netchan_connect(cl, &NC_WEB_PEER);
    flush();                 /* emit the CONNECT once the socket is open */
    return 0;                /* the JS driver takes over from here */
}
