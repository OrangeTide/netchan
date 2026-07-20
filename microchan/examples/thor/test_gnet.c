/* test_gnet.c : host check of the game net layer over UDP loopback */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "game_net.h"
#include "mc_udp.h"
#include <stdio.h>
#include <string.h>
#include <poll.h>

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

/*
 * Wait for either socket to become readable. Loopback delivery is not
 * synchronous everywhere: Linux queues the datagram inside sendto(), while
 * macOS hands it to a separate context, so a non-blocking read right after
 * the send finds nothing. Only the settle loop needs this, because the main
 * loop spins often enough to pick the datagrams up a few iterations later.
 */
static void
wait_pkt(int fd_a, int fd_b, int ms)
{
    struct pollfd p[2];

    p[0].fd = fd_a;
    p[1].fd = fd_b;
    p[0].events = p[1].events = POLLIN;
    p[0].revents = p[1].revents = 0;
    poll(p, 2, ms);
}

int
main(void)
{
    struct mc_udp su, cu;
    struct mc_addr saddr, to, from;
    uint8_t buf[MC_MTU];
    uint32_t now = 0;
    int i, n, spawn_x = -1, spawn_y = -1, moved = 0;

    if (mc_udp_open(&su, "127.0.0.1", 0) || mc_udp_open(&cu, "127.0.0.1", 0)) {
        printf("socket setup failed\n");
        return 2;
    }
    mc_udp_local(&su, &saddr);

    gserver_init(&srv, 0x1234);
    srv.on_chat = on_chat;
    gclient_init(&cli);
    gclient_connect(&cli, &saddr);

    printf("game net layer test (UDP loopback)\n");

    for (i = 0; i < 1500; i++) {
        gserver_service(&srv, now);
        gclient_service(&cli, now);

        if (cli.player >= 0)
            gclient_input(&cli, (uint8_t)((now / 400) % 8));   /* wander */
        if (i == 800 && cli.have_map)
            gclient_chat(&cli, "hello");

        while ((n = (int)gserver_pull(&srv, buf, sizeof(buf), &to)) > 0)
            mc_udp_send(&su, buf, (size_t)n, &to);
        while ((n = (int)gclient_pull(&cli, buf, sizeof(buf), &to)) > 0)
            mc_udp_send(&cu, buf, (size_t)n, &to);
        while ((n = mc_udp_recv(&su, buf, sizeof(buf), &from)) > 0)
            gserver_feed(&srv, buf, (size_t)n, &from);
        while ((n = mc_udp_recv(&cu, buf, sizeof(buf), &from)) > 0)
            gclient_feed(&cli, buf, (size_t)n, &from);

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
     * broadcast yet, or a broadcast still in flight. Stop sending input and
     * pump for several more server ticks so the client holds the final
     * state. GAME_TICK_MS is 125, so 40 iterations covers six ticks.
     */
    for (i = 0; i < 40; i++) {
        gserver_service(&srv, now);
        gclient_service(&cli, now);

        while ((n = (int)gserver_pull(&srv, buf, sizeof(buf), &to)) > 0)
            mc_udp_send(&su, buf, (size_t)n, &to);
        while ((n = (int)gclient_pull(&cli, buf, sizeof(buf), &to)) > 0)
            mc_udp_send(&cu, buf, (size_t)n, &to);
        wait_pkt(su.fd, cu.fd, 10);
        while ((n = mc_udp_recv(&su, buf, sizeof(buf), &from)) > 0)
            gserver_feed(&srv, buf, (size_t)n, &from);
        while ((n = mc_udp_recv(&cu, buf, sizeof(buf), &from)) > 0)
            gclient_feed(&cli, buf, (size_t)n, &from);

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
    mc_udp_close(&su);
    mc_udp_close(&cu);

    printf(g_fail ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
