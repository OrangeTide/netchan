/* game_wire_test.c : reused sim runs, and its state round-trips a snapshot */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "game.h"
#include "game_wire.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL: %s (line %d)\n", #c, __LINE__); fails++; } } while (0)

int
main(void)
{
    struct world w;
    game_init(&w, 0x1234);

    /* two players join and act; run the deterministic sim a while */
    int a = game_add_player(&w);
    int b = game_add_player(&w);
    CHECK(a == 0 && b == 1);
    CHECK(w.nplayers == 2);

    for (int t = 0; t < 40; t++) {
        game_set_input(&w, a, IN_MAKE(2, 4));   /* move east, fire south */
        game_set_input(&w, b, IN_MAKE(6, IN_DIR_NONE));
        game_tick(&w);
    }

    /* pack the dynamic state and reload it into a fresh world */
    uint8_t buf[GW_STATE_SIZE];
    size_t n = gw_pack(&w, buf, sizeof(buf));
    CHECK(n == GW_STATE_SIZE);

    struct world c;
    memset(&c, 0, sizeof(c));
    CHECK(gw_unpack(&c, buf, n) == 0);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        CHECK(c.players[i].x == w.players[i].x);
        CHECK(c.players[i].y == w.players[i].y);
        CHECK(c.players[i].facing == w.players[i].facing);
        CHECK(c.players[i].alive == w.players[i].alive);
        CHECK(c.players[i].hp == w.players[i].hp);
        CHECK(c.players[i].score == w.players[i].score);
    }
    for (int i = 0; i < MAX_CREATURES; i++) {
        CHECK(c.creatures[i].x == w.creatures[i].x);
        CHECK(c.creatures[i].y == w.creatures[i].y);
        CHECK(c.creatures[i].kind == w.creatures[i].kind);
        CHECK(c.creatures[i].alive == w.creatures[i].alive);
    }
    for (int i = 0; i < MAX_SHOTS; i++) {
        CHECK(c.shots[i].x == w.shots[i].x);
        CHECK(c.shots[i].y == w.shots[i].y);
        CHECK(c.shots[i].dir == w.shots[i].dir);
        CHECK(c.shots[i].alive == w.shots[i].alive);
        CHECK(c.shots[i].owner == w.shots[i].owner);
        CHECK(c.shots[i].ttl == w.shots[i].ttl);
    }

    if (fails == 0)
        printf("game sim + snapshot round-trip OK (%zu-byte state)\n", n);
    return fails ? 1 : 0;
}
