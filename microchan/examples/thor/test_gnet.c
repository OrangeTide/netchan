/* test_gnet.c : host check of the game net layer over an in-memory link */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * game_net owns no socket by design: the application pulls datagrams out
 * and feeds them back in. So this drives it over mc_memlink, where delivery
 * is a synchronous function call. The whole check is then deterministic,
 * which matters most for the final state comparison. mc_udp is tested on
 * its own in ../../tests/test_mc_udp.c.
 */

#include "game_net.h"
#include "mc_memlink.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c, m) do { \
        if (c) printf("  ok   %s\n", m); \
        else { printf("  FAIL %s\n", m); g_fail++; } \
    } while (0)

static struct gserver srv;
static struct gclient cli;

static char got_chat[64];
static int got_chat_who = -1;
static void
on_chat(int who, const char *text)
{
    got_chat_who = who;
    strncpy(got_chat, text, sizeof(got_chat) - 1);
}

static struct memlink link;
static struct mc_addr saddr, caddr;

/* Carry both peers one step: whatever each has to send, the other receives
 * before the step is over. */
static void
pump(void)
{
    uint8_t buf[MC_MTU];
    struct mc_addr to, from;
    int n;

    while ((n = (int)gserver_pull(&srv, buf, sizeof(buf), &to)) > 0)
        meml_send(&link, &saddr, buf, (size_t)n, &to);
    while ((n = (int)gclient_pull(&cli, buf, sizeof(buf), &to)) > 0)
        meml_send(&link, &caddr, buf, (size_t)n, &to);
    while ((n = meml_recv(&link, &saddr, buf, sizeof(buf), &from)) > 0)
        gserver_feed(&srv, buf, (size_t)n, &from);
    while ((n = meml_recv(&link, &caddr, buf, sizeof(buf), &from)) > 0)
        gclient_feed(&cli, buf, (size_t)n, &from);
}

int
main(void)
{
    uint32_t now = 0;
    int i, spawn_x = -1, spawn_y = -1, moved = 0;

    meml_init(&link);
    if (meml_open(&link, &saddr) != 0 || meml_open(&link, &caddr) != 0) {
        printf("link setup failed\n");
        return 2;
    }

    gserver_init(&srv, 0x1234);
    srv.on_chat = on_chat;
    gclient_init(&cli);
    gclient_connect(&cli, &saddr);

    printf("game net layer test (in-memory link)\n");

    for (i = 0; i < 1500; i++) {
        gserver_service(&srv, now);
        gclient_service(&cli, now);

        if (cli.player >= 0)
            gclient_input(&cli, (uint8_t)((now / 400) % 8));   /* wander */
        if (i == 800 && cli.have_map)
            gclient_chat(&cli, "hello");

        pump();

        if (cli.have_map && cli.player >= 0 && cli.world.players[cli.player].alive) {
            struct player *p = &cli.world.players[cli.player];
            if (spawn_x < 0) { spawn_x = p->x; spawn_y = p->y; }
            else if (p->x != spawn_x || p->y != spawn_y) moved = 1;
        }
        now += 20;
    }

    /*
     * Settle before comparing world state. The loop above sends input on
     * every iteration, so it always ends with a move the server has not
     * broadcast yet. Stop sending input and pump for a few more server
     * ticks, and the client ends up holding the state the server just
     * simulated. GAME_TICK_MS is 125, so 40 iterations covers six ticks.
     */
    for (i = 0; i < 40; i++) {
        gserver_service(&srv, now);
        gclient_service(&cli, now);
        pump();
        now += 20;
    }

    CHECK(cli.player == 0, "client assigned player index 0");
    CHECK(cli.have_map, "client received full map (MAPEND)");
    CHECK(cli.map_off == (uint16_t)((unsigned)MAP_W * MAP_H % 65536),
          "client received all map bytes");
    CHECK(memcmp(cli.world.tiles, srv.world.tiles, (unsigned)MAP_W * MAP_H) == 0,
          "client map matches server map");
    CHECK(cli.world.players[0].alive, "client sees its player in state");
    CHECK(cli.world.players[0].x == srv.world.players[0].x &&
          cli.world.players[0].y == srv.world.players[0].y,
          "client player position matches server");
    {
        int alive = 0;
        for (i = 0; i < MAX_CREATURES; i++)
            alive += cli.world.creatures[i].alive;
        CHECK(alive > 0, "client sees creatures in state");
    }
    CHECK(moved, "input path works: player moved from spawn");
    CHECK(strcmp(got_chat, "hello") == 0 && got_chat_who == cli.player,
          "chat delivered to server with correct sender");

    gclient_close(&cli);
    gserver_close(&srv);

    printf(g_fail ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
