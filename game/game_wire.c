/* game_wire.c : pack/unpack the dynamic world state for a Snapshot */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "game_wire.h"

size_t
gw_pack(const struct world *w, uint8_t *buf, size_t cap)
{
    if (cap < GW_STATE_SIZE)
        return 0;

    size_t p = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const struct player *pl = &w->players[i];
        buf[p++] = pl->x;
        buf[p++] = pl->y;
        buf[p++] = pl->facing;
        buf[p++] = pl->alive;
        buf[p++] = pl->hp;
        buf[p++] = (uint8_t)(pl->score & 0xff);
        buf[p++] = (uint8_t)(pl->score >> 8);
    }
    for (int i = 0; i < MAX_CREATURES; i++) {
        const struct creature *c = &w->creatures[i];
        buf[p++] = c->x;
        buf[p++] = c->y;
        buf[p++] = c->kind;
        buf[p++] = c->alive;
    }
    for (int i = 0; i < MAX_SHOTS; i++) {
        const struct shot *s = &w->shots[i];
        buf[p++] = s->x;
        buf[p++] = s->y;
        buf[p++] = s->dir;
        buf[p++] = s->alive;
        buf[p++] = s->owner;
        buf[p++] = s->ttl;
    }
    return p;
}

int
gw_unpack(struct world *w, const uint8_t *buf, size_t len)
{
    if (len < GW_STATE_SIZE)
        return -1;

    size_t p = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        struct player *pl = &w->players[i];
        pl->x = buf[p++];
        pl->y = buf[p++];
        pl->facing = buf[p++];
        pl->alive = buf[p++];
        pl->hp = buf[p++];
        pl->score = (uint16_t)(buf[p] | (buf[p + 1] << 8));
        p += 2;
    }
    for (int i = 0; i < MAX_CREATURES; i++) {
        struct creature *c = &w->creatures[i];
        c->x = buf[p++];
        c->y = buf[p++];
        c->kind = buf[p++];
        c->alive = buf[p++];
    }
    for (int i = 0; i < MAX_SHOTS; i++) {
        struct shot *s = &w->shots[i];
        s->x = buf[p++];
        s->y = buf[p++];
        s->dir = buf[p++];
        s->alive = buf[p++];
        s->owner = buf[p++];
        s->ttl = buf[p++];
    }
    return 0;
}
