/* test_game.c : headless sim check, prints ASCII viewport frames (host) */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "game.h"
#include <stdio.h>

#define VW 60
#define VH 22

static struct world w;

static char
glyph_at(int x, int y)
{
    int i;
    for (i = 0; i < MAX_PLAYERS; i++)
        if (w.players[i].alive && w.players[i].x == x && w.players[i].y == y)
            return '@';
    for (i = 0; i < MAX_SHOTS; i++)
        if (w.shots[i].alive && w.shots[i].x == x && w.shots[i].y == y)
            return '*';
    for (i = 0; i < MAX_CREATURES; i++)
        if (w.creatures[i].alive && w.creatures[i].x == x &&
            w.creatures[i].y == y)
            return 'g';
    switch (TILE(&w, x, y)) {
    case T_WALL:     return '#';
    case T_TREASURE: return '$';
    case T_EXIT:     return '>';
    default:         return '.';
    }
}

static void
draw(int tick)
{
    struct player *p = &w.players[0];
    int cx = p->x - VW / 2, cy = p->y - VH / 2, sx, sy, alive = 0, i;

    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx > MAP_W - VW) cx = MAP_W - VW;
    if (cy > MAP_H - VH) cy = MAP_H - VH;

    for (i = 0; i < MAX_CREATURES; i++)
        alive += w.creatures[i].alive;

    printf("--- tick %d  hp=%d score=%d creatures=%d ---\n",
           tick, p->hp, p->score, alive);
    for (sy = 0; sy < VH; sy++) {
        for (sx = 0; sx < VW; sx++)
            putchar(glyph_at(cx + sx, cy + sy));
        putchar('\n');
    }
}

static int
sgn(int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); }

int
main(void)
{
    int t;
    game_init(&w, 0x1234);
    game_add_player(&w);

    for (t = 0; t <= 60; t++) {
        struct player *p = &w.players[0];
        int bd = 0x7fff, bdx = 0, bdy = 0, i;
        /* aim at the nearest creature and fire */
        for (i = 0; i < MAX_CREATURES; i++) {
            struct creature *c = &w.creatures[i];
            int dx, dy, d;
            if (!c->alive)
                continue;
            dx = c->x - p->x;
            dy = c->y - p->y;
            d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            if (d < bd) { bd = d; bdx = dx; bdy = dy; }
        }
        {
            uint8_t in = IN_NONE;
            int k;
            for (k = 0; k < 8; k++)
                if (game_dx[k] == sgn(bdx) && game_dy[k] == sgn(bdy)) {
                    in = IN_MAKE(k, k);     /* move toward and fire at it */
                    break;
                }
            game_set_input(&w, 0, in);
        }
        if (t % 15 == 0)
            draw(t);
        game_tick(&w);
    }
    draw(61);
    return 0;
}
