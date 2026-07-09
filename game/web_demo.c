/* web_demo.c : in-browser Caves-of-Thor over netchan (loopback transport) */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * A self-contained browser build: the authoritative server and the local
 * player's client both run in this wasm module, connected by a netchan
 * loopback (datagrams are handed straight across in-process). This proves
 * the whole client stack, game.c, netchan, and render.c to a canvas, runs
 * in the browser. Replacing the loopback with an nc_web transport to a
 * remote gateway (WebRTC / WebSocket) is the next step and touches only
 * how datagrams leave and enter this module, not the game or the protocol.
 */

#include "netchan.h"
#include "proto.h"
#include "game.h"
#include "game_wire.h"
#include "render.h"
#include "plat.h"

#include <emscripten.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TICK_MS 200

/* a dummy transport address; the loopback has a single, fixed peer */
static struct nc_addr LOOP = { 1, { 1 } };

static struct world w;                 /* the authoritative world */
static struct netchan_conn *sv, *cl;
static struct netchan_chan *sv_wel, *sv_snap, *sv_in;   /* server side */
static struct netchan_chan *cl_in, *cl_wel, *cl_snap;   /* client side */
static struct world cw;                /* the client's view (what we render) */
static int   my_player = -1, have_map = 0, welcomed = 0;
static uint16_t seed;
static uint32_t seq, last_tick, last_input;

/* Move every pending datagram from `from` into `to`. */
static void
pump(struct netchan_conn *from, struct netchan_conn *to)
{
    uint8_t buf[2048];
    struct nc_addr dst;
    for (;;) {
        size_t n = netchan_send_next(from, buf, sizeof(buf), &dst);
        if (n == 0) break;
        netchan_feed(to, buf, n, &LOOP);
    }
}

static uint8_t
read_input(int facing)
{
    static const int8_t dxy[8][2] = {
        {0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1}
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

/* One frame. Exported so the driver (browser requestAnimationFrame, or a
 * headless test harness) stays outside the wasm module. */
EMSCRIPTEN_KEEPALIVE void
web_step(void)
{
    uint32_t now = plat_now_ms();
    plat_poll();

    /* server side: drive channels and consume input */
    struct netchan_event ev;
    while (netchan_poll(sv, &ev)) {
        if (ev.type == NETCHAN_EV_CONNECTED) {
            my_player = game_add_player(&w);
            sv_wel = netchan_chan_open(sv, NETCHAN_RELIABLE, NETCHAN_DIR_SEND, "welcome");
            sv_snap = netchan_chan_open(sv, NETCHAN_UNRELIABLE, NETCHAN_DIR_SEND, "snap");
        } else if (ev.type == NETCHAN_EV_CHAN_OPEN) {
            sv_in = ev.ch;
        } else if (ev.type == NETCHAN_EV_DATA && ev.ch == sv_in) {
            uint8_t b[64];
            int rd = netchan_chan_read(ev.ch, b, sizeof(b));
            struct player_input pi;
            if (rd > 0 && player_input_decode(&pi, b, rd) > 0 && my_player >= 0)
                game_set_input(&w, my_player, pi.input);
        }
    }
    if (!welcomed && sv_wel && netchan_chan_state(sv_wel) == 1 && my_player >= 0) {
        struct welcome wl = { .your_player = (uint16_t)my_player, .seed = seed };
        uint8_t b[32];
        int m = welcome_encode(&wl, b, sizeof(b));
        if (m > 0 && netchan_chan_write(sv_wel, b, m) == m) welcomed = 1;
    }

    /* client side: open input channel, take welcome + snapshots */
    if (!cl_in && netchan_state(cl) == NETCHAN_STATE_CONNECTED)
        cl_in = netchan_chan_open(cl, NETCHAN_UNRELIABLE, NETCHAN_DIR_SEND, "input");
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
                }
            }
        }
    }

    /* client sends input at ~15 Hz */
    if (cl_in && netchan_chan_state(cl_in) == 1 && now - last_input >= 66) {
        last_input = now;
        int facing = (have_map && my_player >= 0) ? cw.players[my_player].facing : 0;
        struct player_input pi = { .seq = ++seq, .input = read_input(facing) };
        uint8_t b[32];
        int m = player_input_encode(&pi, b, sizeof(b));
        if (m > 0) netchan_chan_write(cl_in, b, m);
    }

    /* fixed-rate server tick + snapshot */
    if (now - last_tick >= TICK_MS) {
        last_tick += TICK_MS;
        if (last_tick + TICK_MS < now) last_tick = now;   /* don't spiral */
        game_tick(&w);
        uint8_t state[GW_STATE_SIZE];
        gw_pack(&w, state, sizeof(state));
        struct snapshot sn = { .tick = w.tick, .state = state,
                               .state_len = (uint16_t)sizeof(state) };
        uint8_t b[512];
        int m = snapshot_encode(&sn, b, sizeof(b));
        if (sv_snap && netchan_chan_state(sv_snap) == 1 && m > 0)
            netchan_chan_write(sv_snap, b, m);
    }

    netchan_service(sv, now);
    netchan_service(cl, now);
    pump(cl, sv);      /* deliver client -> server */
    pump(sv, cl);      /* deliver server -> client */

    if (have_map && my_player >= 0) {
        char hud[80];
        const struct player *me = &cw.players[my_player];
        snprintf(hud, sizeof(hud), "hp %2u  score %u  tick %u  arrows move, WASD fire",
                 me->hp, me->score, cw.tick);
        render_world(&cw, my_player, hud);
    }
}

int
main(void)
{
    seed = 0x1234;
    game_init(&w, seed);
    plat_init();

    sv = netchan_open(1);
    cl = netchan_open(0);
    netchan_connect(cl, &LOOP);

    /* the server accepts on its first received datagram */
    pump(cl, sv);
    netchan_accept(sv);
    pump(sv, cl);

    last_tick = plat_now_ms();
    return 0;   /* setup done; the driver calls web_step() each frame */
}
