/* game_net.c : microchan wire protocol and peer glue for the game */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "game_net.h"
#include <string.h>

#define MAP_BYTES  ((unsigned)MAP_W * MAP_H)
#define MAP_CHUNK  512

/****************************************************************
 * Byte I/O (big-endian) and addressing helpers
 ****************************************************************/

static void
wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t
rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int
addr_eq(const struct mc_addr *a, const struct mc_addr *b)
{
    return a->len == b->len && memcmp(a->a, b->a, a->len) == 0;
}

/****************************************************************
 * State serialization
 ****************************************************************/

static size_t
serialize_state(const struct world *w, uint8_t *b)
{
    uint8_t *p = b;
    uint8_t *cnt;
    int i;

    *p++ = GM_STATE;
    wr16(p, w->tick);
    p += 2;

    for (i = 0; i < MAX_PLAYERS; i++) {
        const struct player *pl = &w->players[i];
        *p++ = pl->alive;
        *p++ = pl->x;
        *p++ = pl->y;
        *p++ = pl->facing;
        *p++ = pl->hp;
        wr16(p, pl->score);
        p += 2;
    }

    cnt = p++;
    *cnt = 0;
    for (i = 0; i < MAX_CREATURES; i++) {
        const struct creature *c = &w->creatures[i];
        if (!c->alive)
            continue;
        *p++ = c->x;
        *p++ = c->y;
        *p++ = c->kind;
        (*cnt)++;
    }

    cnt = p++;
    *cnt = 0;
    for (i = 0; i < MAX_SHOTS; i++) {
        const struct shot *s = &w->shots[i];
        if (!s->alive)
            continue;
        *p++ = s->x;
        *p++ = s->y;
        *p++ = s->dir;
        (*cnt)++;
    }

    return (size_t)(p - b);
}

static void
deserialize_state(struct world *w, const uint8_t *b, size_t len)
{
    const uint8_t *p = b + 1;   /* skip GM_STATE */
    int i, n;

    if (len < 3 + MAX_PLAYERS * 7)
        return;
    w->tick = rd16(p);
    p += 2;

    for (i = 0; i < MAX_PLAYERS; i++) {
        struct player *pl = &w->players[i];
        pl->alive = *p++;
        pl->x = *p++;
        pl->y = *p++;
        pl->facing = *p++;
        pl->hp = *p++;
        pl->score = rd16(p);
        p += 2;
    }

    for (i = 0; i < MAX_CREATURES; i++)
        w->creatures[i].alive = 0;
    n = *p++;
    for (i = 0; i < n && i < MAX_CREATURES; i++) {
        w->creatures[i].x = *p++;
        w->creatures[i].y = *p++;
        w->creatures[i].kind = *p++;
        w->creatures[i].alive = 1;
    }

    for (i = 0; i < MAX_SHOTS; i++)
        w->shots[i].alive = 0;
    n = *p++;
    for (i = 0; i < n && i < MAX_SHOTS; i++) {
        w->shots[i].x = *p++;
        w->shots[i].y = *p++;
        w->shots[i].dir = *p++;
        w->shots[i].alive = 1;
    }
}

/****************************************************************
 * Server
 ****************************************************************/

void
gserver_init(struct gserver *s, uint16_t seed)
{
    memset(s, 0, sizeof(*s));
    game_init(&s->world, seed);
}

static struct gconn *
find_by_addr(struct gserver *s, const struct mc_addr *a)
{
    int i;
    for (i = 0; i < MAX_PLAYERS; i++)
        if (s->conn[i].used && addr_eq(&s->conn[i].addr, a))
            return &s->conn[i];
    return NULL;
}

static struct gconn *
find_by_id(struct gserver *s, uint32_t id)
{
    int i;
    for (i = 0; i < MAX_PLAYERS; i++)
        if (s->conn[i].used && mc_id(s->conn[i].nc) == id)
            return &s->conn[i];
    return NULL;
}

static struct gconn *
accept_conn(struct gserver *s, const struct mc_addr *from)
{
    int i;
    for (i = 0; i < MAX_PLAYERS; i++) {
        struct gconn *gc = &s->conn[i];
        if (gc->used)
            continue;
        gc->nc = mc_open(1);
        if (!gc->nc)
            return NULL;
        gc->rel = mc_chan_open(gc->nc, MC_RELIABLE);
        gc->unr = mc_chan_open(gc->nc, MC_UNRELIABLE);
        gc->addr = *from;
        gc->player = -1;
        gc->map_off = 0;
        gc->mapped = 0;
        gc->used = 1;
        return gc;
    }
    return NULL;
}

void
gserver_feed(struct gserver *s, const void *pkt, size_t len,
             const struct mc_addr *from)
{
    uint32_t id = mc_peek_id(pkt, len);
    struct gconn *gc = id ? find_by_id(s, id) : find_by_addr(s, from);
    if (!gc && id == 0)
        gc = accept_conn(s, from);
    if (gc)
        mc_feed(gc->nc, pkt, len, from);
}

static void
stream_map(struct gserver *s, struct gconn *gc)
{
    uint8_t buf[5 + MAP_CHUNK];
    while (gc->map_off < MAP_BYTES) {
        unsigned chunk = MAP_BYTES - gc->map_off;
        if (chunk > MAP_CHUNK)
            chunk = MAP_CHUNK;
        buf[0] = GM_MAP;
        wr16(buf + 1, gc->map_off);
        wr16(buf + 3, (uint16_t)chunk);
        memcpy(buf + 5, &s->world.tiles[gc->map_off], chunk);
        if (mc_write(gc->rel, buf, 5 + chunk) == MC_ERR_AGAIN)
            return;             /* window full; resume next service */
        gc->map_off = (uint16_t)(gc->map_off + chunk);
    }
    buf[0] = GM_MAPEND;
    if (mc_write(gc->rel, buf, 1) != MC_ERR_AGAIN)
        gc->mapped = 1;
}

static void
server_drain(struct gserver *s, struct gconn *gc)
{
    uint8_t msg[MC_MTU];
    int n;

    while ((n = mc_read(gc->rel, msg, sizeof(msg))) > 0) {
        if (msg[0] == GM_CHAT && n >= 3) {
            int i, who = gc->player, clen = msg[2];
            msg[1] = (uint8_t)who;          /* stamp the real sender */
            for (i = 0; i < MAX_PLAYERS; i++)
                if (s->conn[i].used && s->conn[i].player >= 0)
                    mc_write(s->conn[i].rel, msg, (size_t)n);
            if (3 + clen > n)
                clen = n - 3;
            msg[3 + clen] = 0;              /* terminate for the callback */
            if (s->on_chat)
                s->on_chat(who, (char *)msg + 3);
        }
    }
    while ((n = mc_read(gc->unr, msg, sizeof(msg))) > 0) {
        if (msg[0] == GM_INPUT && n >= 2 && gc->player >= 0)
            game_set_input(&s->world, gc->player, msg[1]);
    }
}

static void
server_close(struct gconn *gc)
{
    if (gc->nc)
        mc_close(gc->nc);
    memset(gc, 0, sizeof(*gc));
    gc->player = -1;
}

void
gserver_service(struct gserver *s, uint32_t now_ms)
{
    uint8_t buf[MC_MTU];
    int i;
    size_t slen;

    for (i = 0; i < MAX_PLAYERS; i++) {
        struct gconn *gc = &s->conn[i];
        struct mc_event ev;
        if (!gc->used)
            continue;
        mc_service(gc->nc, now_ms);
        while (mc_poll(gc->nc, &ev)) {
            if (ev.type == MC_EV_CONNECTED) {
                gc->player = game_add_player(&s->world);
                buf[0] = GM_WELCOME;
                buf[1] = (uint8_t)gc->player;
                mc_write(gc->rel, buf, 2);
                gc->map_off = 0;
                gc->mapped = 0;
            } else if (ev.type == MC_EV_DISCONNECTED) {
                if (gc->player >= 0)
                    s->world.players[gc->player].alive = 0;
                server_close(gc);
            }
        }
        if (!gc->used)
            continue;
        server_drain(s, gc);
        if (gc->player >= 0 && !gc->mapped)
            stream_map(s, gc);
    }

    if (now_ms - s->last_tick_ms >= GAME_TICK_MS) {
        s->last_tick_ms = now_ms;
        game_tick(&s->world);
        slen = serialize_state(&s->world, buf);
        for (i = 0; i < MAX_PLAYERS; i++) {
            struct gconn *gc = &s->conn[i];
            if (gc->used && gc->player >= 0 && gc->mapped)
                mc_write(gc->unr, buf, slen);
        }
    }
}

void
gserver_say(struct gserver *s, int who, const char *text)
{
    uint8_t b[3 + CHAT_MAX];
    size_t len = 0;
    int i;
    while (len < CHAT_MAX && text[len])
        len++;
    b[0] = GM_CHAT;
    b[1] = (uint8_t)who;
    b[2] = (uint8_t)len;
    memcpy(b + 3, text, len);
    for (i = 0; i < MAX_PLAYERS; i++)
        if (s->conn[i].used && s->conn[i].player >= 0)
            mc_write(s->conn[i].rel, b, 3 + len);
    if (s->on_chat)
        s->on_chat(who, text);
}

void
gserver_close(struct gserver *s)
{
    int i;
    for (i = 0; i < MAX_PLAYERS; i++)
        if (s->conn[i].used)
            server_close(&s->conn[i]);
}

size_t
gserver_pull(struct gserver *s, void *buf, size_t buflen, struct mc_addr *to)
{
    int i;
    for (i = 0; i < MAX_PLAYERS; i++) {
        struct gconn *gc = &s->conn[i];
        size_t n;
        if (!gc->used)
            continue;
        n = mc_send_next(gc->nc, buf, buflen, to);
        if (n)
            return n;
    }
    return 0;
}

/****************************************************************
 * Client
 ****************************************************************/

void
gclient_init(struct gclient *c)
{
    memset(c, 0, sizeof(*c));
    c->nc = mc_open(0);
    c->rel = mc_chan_open(c->nc, MC_RELIABLE);
    c->unr = mc_chan_open(c->nc, MC_UNRELIABLE);
    c->player = -1;
}

int
gclient_connect(struct gclient *c, const struct mc_addr *server)
{
    return mc_connect(c->nc, server);
}

void
gclient_feed(struct gclient *c, const void *pkt, size_t len,
             const struct mc_addr *from)
{
    mc_feed(c->nc, pkt, len, from);
}

static void
client_drain(struct gclient *c)
{
    uint8_t msg[MC_MTU];
    int n;

    while ((n = mc_read(c->rel, msg, sizeof(msg))) > 0) {
        switch (msg[0]) {
        case GM_WELCOME:
            if (n >= 2)
                c->player = (int8_t)msg[1];
            break;
        case GM_MAP:
            if (n >= 5) {
                unsigned off = rd16(msg + 1), ln = rd16(msg + 3);
                if (off + ln <= MAP_BYTES && (int)(5 + ln) <= n) {
                    memcpy(&c->world.tiles[off], msg + 5, ln);
                    c->map_off = (uint16_t)(off + ln);
                }
            }
            break;
        case GM_MAPEND:
            c->have_map = 1;
            break;
        case GM_CHAT:
            if (n >= 3 && c->on_chat) {
                int clen = msg[2];
                if (3 + clen > n)
                    clen = n - 3;
                msg[3 + clen] = 0;
                c->on_chat(msg[1], (char *)msg + 3);
            }
            break;
        default:
            break;
        }
    }
    while ((n = mc_read(c->unr, msg, sizeof(msg))) > 0) {
        if (msg[0] == GM_STATE)
            deserialize_state(&c->world, msg, (size_t)n);
    }
}

void
gclient_service(struct gclient *c, uint32_t now_ms)
{
    struct mc_event ev;
    mc_service(c->nc, now_ms);
    while (mc_poll(c->nc, &ev))
        ;                       /* connection events not needed by the app */
    client_drain(c);
}

size_t
gclient_pull(struct gclient *c, void *buf, size_t buflen, struct mc_addr *to)
{
    return mc_send_next(c->nc, buf, buflen, to);
}

void
gclient_input(struct gclient *c, uint8_t input)
{
    uint8_t b[2];
    b[0] = GM_INPUT;
    b[1] = input;
    mc_write(c->unr, b, 2);
}

void
gclient_chat(struct gclient *c, const char *text)
{
    uint8_t b[3 + CHAT_MAX];
    size_t len = 0;
    while (len < CHAT_MAX && text[len])
        len++;
    b[0] = GM_CHAT;
    b[1] = (uint8_t)c->player;
    b[2] = (uint8_t)len;
    memcpy(b + 3, text, len);
    mc_write(c->rel, b, 3 + len);
}

void
gclient_close(struct gclient *c)
{
    if (!c->nc)
        return;
    mc_close(c->nc);
    c->nc = NULL;
    c->rel = NULL;
    c->unr = NULL;
}
