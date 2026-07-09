/* render.c : text-mode rendering of the game world (see render.h) */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "render.h"
#include "plat.h"
#include <stdio.h>
#include <string.h>

#define VIEW_H (SCR_H - 1)      /* bottom row is the HUD */

/* distinct colors for the four players; the local player is drawn brighter */
static const uint8_t player_fg[MAX_PLAYERS] = {
    C_YELLOW, C_LCYAN, C_LGREEN, C_LMAGENTA,
};

/* projectile glyph by travel direction (index with shot dir, 0..7 clockwise
 * from north). CP437 has cardinal arrows at 0x18..0x1B; the diagonals have no
 * arrow glyph, so they borrow the slashes that point the same way. */
static const unsigned char shot_glyph[8] = {
    0x18,   /* N  up    */
    '/',    /* NE       */
    0x1A,   /* E  right */
    '\\',   /* SE       */
    0x19,   /* S  down  */
    '/',    /* SW       */
    0x1B,   /* W  left  */
    '\\',   /* NW       */
};

static int cam_x, cam_y;

static void
put_str(int x, int y, const char *s, uint8_t attr)
{
    while (*s) {
        plat_put(x++, y, (unsigned char)*s, attr);
        s++;
    }
}

static void
glyph_for_tile(uint8_t t, unsigned char *ch, uint8_t *attr)
{
    switch (t) {
    case T_WALL:     *ch = 0xB1; *attr = ATTR(C_GREY, C_BLACK);   break;
    case T_TREASURE: *ch = '$';  *attr = ATTR(C_YELLOW, C_BLACK); break;
    case T_EXIT:     *ch = '>';  *attr = ATTR(C_WHITE, C_BLACK);  break;
    default:         *ch = 0xFA; *attr = ATTR(C_DGREY, C_BLACK);  break;
    }
}

void
render_world(const struct world *w, int self, const char *status)
{
    const struct player *me = &w->players[self];
    int sx, sy, i;
    char hud[SCR_W + 1];

    if (me->alive) {
        cam_x = me->x - SCR_W / 2;
        cam_y = me->y - VIEW_H / 2;
        if (cam_x < 0) cam_x = 0;
        if (cam_y < 0) cam_y = 0;
        if (cam_x > MAP_W - SCR_W) cam_x = MAP_W - SCR_W;
        if (cam_y > MAP_H - VIEW_H) cam_y = MAP_H - VIEW_H;
    }

    /* tiles */
    for (sy = 0; sy < VIEW_H; sy++) {
        for (sx = 0; sx < SCR_W; sx++) {
            unsigned char ch;
            uint8_t attr;
            glyph_for_tile(TILE(w, cam_x + sx, cam_y + sy), &ch, &attr);
            plat_put(sx, sy, ch, attr);
        }
    }

    /* creatures */
    for (i = 0; i < MAX_CREATURES; i++) {
        const struct creature *c = &w->creatures[i];
        int x = c->x - cam_x, y = c->y - cam_y;
        if (c->alive && x >= 0 && x < SCR_W && y >= 0 && y < VIEW_H)
            plat_put(x, y, 0x01, ATTR(C_LRED, C_BLACK));
    }

    /* shots */
    for (i = 0; i < MAX_SHOTS; i++) {
        const struct shot *s = &w->shots[i];
        int x = s->x - cam_x, y = s->y - cam_y;
        if (s->alive && x >= 0 && x < SCR_W && y >= 0 && y < VIEW_H)
            plat_put(x, y, shot_glyph[s->dir & 7], ATTR(C_WHITE, C_BLACK));
    }

    /* players */
    for (i = 0; i < MAX_PLAYERS; i++) {
        const struct player *p = &w->players[i];
        int x = p->x - cam_x, y = p->y - cam_y;
        if (p->alive && x >= 0 && x < SCR_W && y >= 0 && y < VIEW_H)
            plat_put(x, y, '@', ATTR(player_fg[i], i == self ? C_BLUE : C_BLACK));
    }

    /* HUD */
    if (status && *status) {
        memset(hud, ' ', SCR_W);
        hud[SCR_W] = 0;
        put_str(0, SCR_H - 1, status, ATTR(C_WHITE, C_BLACK));
        for (i = (int)strlen(status); i < SCR_W; i++)
            plat_put(i, SCR_H - 1, ' ', ATTR(C_WHITE, C_BLACK));
    } else {
        snprintf(hud, sizeof(hud), "P%d  HP:%2d  SCORE:%u",
                 self + 1, me->hp, me->score);
        for (i = 0; hud[i]; i++)
            plat_put(i, SCR_H - 1, (unsigned char)hud[i],
                     ATTR(C_BLACK, C_GREY));
        for (; i < SCR_W; i++)
            plat_put(i, SCR_H - 1, ' ', ATTR(C_BLACK, C_GREY));
    }

    plat_present();
}
