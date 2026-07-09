/* game.c : portable server-authoritative game core (see game.h) */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "game.h"
#include "rng.h"
#include <string.h>

/* 8 directions, clockwise from north */
const int8_t game_dx[8] = {  0,  1, 1, 1, 0, -1, -1, -1 };
const int8_t game_dy[8] = { -1, -1, 0, 1, 1,  1,  0, -1 };

#define SHOT_SPEED   2
#define FIRE_CD      2          /* ticks between shots from one player */
#define RESPAWN_T    24         /* ticks a dead player waits to respawn */
#define CREATURE_DMG 1

/* server-only per-player state (not part of the broadcast) */
static uint8_t fire_cd[MAX_PLAYERS];
static uint8_t respawn_t[MAX_PLAYERS];

static int
sgn(int v)
{
    return v < 0 ? -1 : (v > 0 ? 1 : 0);
}

static int
vec_to_dir(int dx, int dy)
{
    int i;
    dx = sgn(dx);
    dy = sgn(dy);
    for (i = 0; i < 8; i++)
        if (game_dx[i] == dx && game_dy[i] == dy)
            return i;
    return -1;
}

static int
is_floor(struct world *w, int x, int y)
{
    if (x < 0 || y < 0 || x >= MAP_W || y >= MAP_H)
        return 0;
    return TILE(w, x, y) != T_WALL;
}

/****************************************************************
 * Map generation: a cellular-automata cave
 ****************************************************************/

static int
wall_neighbors(struct world *w, int x, int y)
{
    int dx, dy, n = 0;
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++) {
            if (!dx && !dy)
                continue;
            if (TILE(w, x + dx, y + dy) == T_WALL)
                n++;
        }
    return n;
}

/* flood-fill scratch, sized for the map; static to keep it off the stack */
static uint8_t seen[MAP_W * MAP_H];
static uint16_t stack[MAP_W * MAP_H];

static void
keep_largest_region(struct world *w)
{
    int x, y, best_start = -1, best_size = 0;

    memset(seen, 0, sizeof(seen));
    for (y = 1; y < MAP_H - 1; y++) {
        for (x = 1; x < MAP_W - 1; x++) {
            int start = y * MAP_W + x, size = 0, sp = 0;
            if (TILE(w, x, y) == T_WALL || seen[start])
                continue;
            stack[sp++] = (uint16_t)start;
            seen[start] = 1;
            while (sp) {
                int cur = stack[--sp];
                int cx = cur % MAP_W, cy = cur / MAP_W, d;
                size++;
                for (d = 0; d < 8; d += 2) {     /* 4-connectivity */
                    int nx = cx + game_dx[d], ny = cy + game_dy[d];
                    int ni = ny * MAP_W + nx;
                    if (nx < 1 || ny < 1 || nx >= MAP_W - 1 || ny >= MAP_H - 1)
                        continue;
                    if (TILE(w, nx, ny) == T_WALL || seen[ni])
                        continue;
                    seen[ni] = 1;
                    stack[sp++] = (uint16_t)ni;
                }
            }
            if (size > best_size) {
                best_size = size;
                best_start = start;
            }
        }
    }

    /* re-flood the largest region, then wall off everything else */
    memset(seen, 0, sizeof(seen));
    if (best_start >= 0) {
        int sp = 0;
        stack[sp++] = (uint16_t)best_start;
        seen[best_start] = 1;
        while (sp) {
            int cur = stack[--sp];
            int cx = cur % MAP_W, cy = cur / MAP_W, d;
            for (d = 0; d < 8; d += 2) {
                int nx = cx + game_dx[d], ny = cy + game_dy[d];
                int ni = ny * MAP_W + nx;
                if (nx < 1 || ny < 1 || nx >= MAP_W - 1 || ny >= MAP_H - 1)
                    continue;
                if (TILE(w, nx, ny) == T_WALL || seen[ni])
                    continue;
                seen[ni] = 1;
                stack[sp++] = (uint16_t)ni;
            }
        }
    }
    for (y = 0; y < MAP_H; y++)
        for (x = 0; x < MAP_W; x++)
            if (TILE(w, x, y) != T_WALL && !seen[y * MAP_W + x])
                TILE(w, x, y) = T_WALL;
}

static void
gen_map(struct world *w)
{
    int x, y, pass;
    static uint8_t next[MAP_W * MAP_H];

    for (y = 0; y < MAP_H; y++)
        for (x = 0; x < MAP_W; x++) {
            if (x == 0 || y == 0 || x == MAP_W - 1 || y == MAP_H - 1)
                TILE(w, x, y) = T_WALL;
            else
                TILE(w, x, y) = (rng_range(100) < 45) ? T_WALL : T_FLOOR;
        }

    for (pass = 0; pass < 4; pass++) {
        for (y = 1; y < MAP_H - 1; y++)
            for (x = 1; x < MAP_W - 1; x++) {
                int n = wall_neighbors(w, x, y);
                next[y * MAP_W + x] = (uint8_t)(n >= 5 ? T_WALL : T_FLOOR);
            }
        for (y = 1; y < MAP_H - 1; y++)
            for (x = 1; x < MAP_W - 1; x++)
                TILE(w, x, y) = next[y * MAP_W + x];
    }

    keep_largest_region(w);
}

static int
random_floor(struct world *w, uint8_t *ox, uint8_t *oy)
{
    int tries;
    for (tries = 0; tries < 4000; tries++) {
        int x = 1 + (int)rng_range(MAP_W - 2);
        int y = 1 + (int)rng_range(MAP_H - 2);
        if (TILE(w, x, y) == T_FLOOR) {
            *ox = (uint8_t)x;
            *oy = (uint8_t)y;
            return 1;
        }
    }
    return 0;
}

/****************************************************************
 * Setup
 ****************************************************************/

void
game_init(struct world *w, uint16_t seed)
{
    int i;
    memset(w, 0, sizeof(*w));
    memset(fire_cd, 0, sizeof(fire_cd));
    memset(respawn_t, 0, sizeof(respawn_t));
    rng_seed((uint8_t)seed, (uint8_t)(seed >> 8), 0x5A);

    gen_map(w);

    for (i = 0; i < MAX_CREATURES; i++) {
        struct creature *c = &w->creatures[i];
        if (random_floor(w, &c->x, &c->y)) {
            c->kind = 0;
            c->alive = 1;
        }
    }
}

int
game_add_player(struct world *w)
{
    int i;
    for (i = 0; i < MAX_PLAYERS; i++) {
        struct player *p = &w->players[i];
        if (!p->alive && p->hp == 0 && respawn_t[i] == 0) {
            if (!random_floor(w, &p->x, &p->y))
                return -1;
            p->facing = 4;
            p->alive = 1;
            p->hp = PLAYER_HP;
            p->input = IN_NONE;
            p->score = 0;
            if (i + 1 > w->nplayers)
                w->nplayers = (uint8_t)(i + 1);
            return i;
        }
    }
    return -1;
}

void
game_set_input(struct world *w, int player, uint8_t input)
{
    if (player >= 0 && player < MAX_PLAYERS)
        w->players[player].input = input;
}

/****************************************************************
 * Simulation
 ****************************************************************/

static struct player *
nearest_player(struct world *w, int cx, int cy)
{
    struct player *best = NULL;
    int best_d = 0x7fff, i;
    for (i = 0; i < MAX_PLAYERS; i++) {
        struct player *p = &w->players[i];
        int ax, ay, d;
        if (!p->alive)
            continue;
        ax = p->x - cx;
        if (ax < 0) ax = -ax;
        ay = p->y - cy;
        if (ay < 0) ay = -ay;
        d = ax > ay ? ax : ay;
        if (d < best_d) {
            best_d = d;
            best = p;
        }
    }
    return best;
}

static void
step_players(struct world *w)
{
    int i;
    for (i = 0; i < MAX_PLAYERS; i++) {
        struct player *p = &w->players[i];
        uint8_t dir, fdir;
        if (!p->alive)
            continue;
        if (fire_cd[i])
            fire_cd[i]--;
        dir = (uint8_t)IN_DIR(p->input);
        if (dir < 8) {
            int nx = p->x + game_dx[dir], ny = p->y + game_dy[dir];
            p->facing = dir;
            if (is_floor(w, nx, ny)) {
                p->x = (uint8_t)nx;
                p->y = (uint8_t)ny;
                if (TILE(w, nx, ny) == T_TREASURE) {
                    TILE(w, nx, ny) = T_FLOOR;
                    p->score += 25;
                }
            }
        }
        fdir = (uint8_t)IN_FIRE_DIR(p->input);
        if (fdir < 8 && fire_cd[i] == 0) {
            int s;
            for (s = 0; s < MAX_SHOTS; s++) {
                struct shot *sh = &w->shots[s];
                if (!sh->alive) {
                    sh->x = p->x;
                    sh->y = p->y;
                    sh->dir = fdir;
                    sh->owner = (uint8_t)i;
                    sh->ttl = SHOT_RANGE;
                    sh->alive = 1;
                    fire_cd[i] = FIRE_CD;
                    break;
                }
            }
        }
    }
}

static struct creature *
creature_at(struct world *w, int x, int y)
{
    int i;
    for (i = 0; i < MAX_CREATURES; i++) {
        struct creature *c = &w->creatures[i];
        if (c->alive && c->x == x && c->y == y)
            return c;
    }
    return NULL;
}

static void
step_shots(struct world *w)
{
    int s, step;
    for (s = 0; s < MAX_SHOTS; s++) {
        struct shot *sh = &w->shots[s];
        if (!sh->alive)
            continue;
        for (step = 0; step < SHOT_SPEED; step++) {
            int nx = sh->x + game_dx[sh->dir], ny = sh->y + game_dy[sh->dir];
            struct creature *c;
            if (!is_floor(w, nx, ny)) {
                sh->alive = 0;
                break;
            }
            sh->x = (uint8_t)nx;
            sh->y = (uint8_t)ny;
            c = creature_at(w, nx, ny);
            if (c) {
                c->alive = 0;
                if (sh->owner < MAX_PLAYERS)
                    w->players[sh->owner].score += 10;
                sh->alive = 0;
                break;
            }
            if (sh->ttl == 0) {
                sh->alive = 0;
                break;
            }
            sh->ttl--;
        }
    }
}

static void
step_creatures(struct world *w)
{
    int i;
    if (w->tick & 1)
        return;                 /* creatures move at half the player rate */
    for (i = 0; i < MAX_CREATURES; i++) {
        struct creature *c = &w->creatures[i];
        struct player *p;
        int dx, dy, dir, nx, ny;
        if (!c->alive)
            continue;
        p = nearest_player(w, c->x, c->y);
        if (!p)
            continue;
        dx = p->x - c->x;
        dy = p->y - c->y;
        if ((dx >= -1 && dx <= 1) && (dy >= -1 && dy <= 1)) {
            /* adjacent (or on top): attack */
            if (p->hp > CREATURE_DMG)
                p->hp -= CREATURE_DMG;
            else {
                p->hp = 0;
                p->alive = 0;
                respawn_t[p - w->players] = RESPAWN_T;
            }
            continue;
        }
        dir = vec_to_dir(dx, dy);
        if (dir < 0)
            continue;
        nx = c->x + game_dx[dir];
        ny = c->y + game_dy[dir];
        if (is_floor(w, nx, ny) && !creature_at(w, nx, ny)) {
            c->x = (uint8_t)nx;
            c->y = (uint8_t)ny;
        } else {
            /* slide along one axis if the diagonal is blocked */
            if (is_floor(w, c->x + sgn(dx), c->y) &&
                !creature_at(w, c->x + sgn(dx), c->y))
                c->x = (uint8_t)(c->x + sgn(dx));
            else if (is_floor(w, c->x, c->y + sgn(dy)) &&
                     !creature_at(w, c->x, c->y + sgn(dy)))
                c->y = (uint8_t)(c->y + sgn(dy));
        }
    }
}

static void
step_respawns(struct world *w)
{
    int i;
    for (i = 0; i < MAX_PLAYERS; i++) {
        if (!w->players[i].alive && respawn_t[i] > 0) {
            if (--respawn_t[i] == 0) {
                struct player *p = &w->players[i];
                if (random_floor(w, &p->x, &p->y)) {
                    p->alive = 1;
                    p->hp = PLAYER_HP;
                }
            }
        }
    }
}

void
game_tick(struct world *w)
{
    step_players(w);
    step_shots(w);
    step_creatures(w);
    step_respawns(w);
    w->tick++;
}
