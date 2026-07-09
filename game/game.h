/* game.h : portable server-authoritative game core for the netchan demo */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * A small Gauntlet-style game with a Caves-of-Thor text-mode look: a larger
 * tile world than the screen, a handful of creatures that step toward the
 * nearest player, travelling projectiles, and up to four players. The core
 * is transport-agnostic and deterministic on the server; clients render the
 * state the server broadcasts. Entities live on integer tiles and step once
 * per tick.
 */

#ifndef GAME_H
#define GAME_H

#include <stdint.h>

#define MAP_W        96
#define MAP_H        64
#define MAX_PLAYERS  4
#define MAX_CREATURES 24
#define MAX_SHOTS    16
#define SHOT_RANGE   6          /* tiles a projectile travels before dying */
#define PLAYER_HP    10

/* tiles */
enum {
    T_WALL = 0,
    T_FLOOR,
    T_EXIT,
    T_TREASURE,
};

/* input byte: low nibble = move direction, high nibble = fire direction.
 * Each nibble holds a direction 0..7, or IN_DIR_NONE for "none". The fire
 * direction is independent of movement, so the controls are twin-stick:
 * one set of keys steers, another set aims. IN_NONE is the idle byte. */
#define IN_DIR_NONE 0x0F
#define IN_NONE     0xFF
#define IN_DIR(b)      ((b) & 0x0F)
#define IN_FIRE_DIR(b) (((b) >> 4) & 0x0F)
#define IN_MAKE(move, fire) \
    (uint8_t)(((move) & 0x0F) | (((fire) & 0x0F) << 4))

/* 8 directions, clockwise from north; index with dir */
extern const int8_t game_dx[8];
extern const int8_t game_dy[8];

struct player {
    uint8_t x, y;
    uint8_t facing;             /* 0..7 */
    uint8_t alive;
    uint8_t hp;
    uint8_t input;              /* latest input byte from this client */
    uint16_t score;
};

struct creature {
    uint8_t x, y;
    uint8_t kind;
    uint8_t alive;
};

struct shot {
    uint8_t x, y;
    uint8_t dir;
    uint8_t alive;
    uint8_t owner;
    uint8_t ttl;
};

struct world {
    uint8_t tiles[MAP_W * MAP_H];
    struct player players[MAX_PLAYERS];
    struct creature creatures[MAX_CREATURES];
    struct shot shots[MAX_SHOTS];
    uint16_t tick;
    uint8_t nplayers;
};

#define TILE(w, x, y) ((w)->tiles[(unsigned)(y) * MAP_W + (unsigned)(x)])

/****************************************************************
 * Server-authoritative simulation
 ****************************************************************/

/** Generate the map and spawn creatures from a seed (server side). */
void game_init(struct world *w, uint16_t seed);

/** Add a player at a free floor tile; returns its index or -1. */
int game_add_player(struct world *w);

/** Record the latest input for a player. */
void game_set_input(struct world *w, int player, uint8_t input);

/** Advance the simulation by one tick. */
void game_tick(struct world *w);

#endif /* GAME_H */
