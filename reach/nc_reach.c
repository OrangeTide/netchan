/*
 * nc_reach.c : the hole-punch session and packet (see reach/nc_reach.h)
 *
 * Made by a machine.
 *
 * Two peers behind NATs, each knowing the other's public address, open a path
 * by firing at each other at once. This file derives the session both sides
 * share and builds and parses the one punch packet. It opens no socket and runs
 * no timer; the application owns the cadence and the clock.
 */

#include "nc_reach.h"

#include <string.h>

/* Order two addresses the same way on both peers, so each derives the same
 * session from the pair. A shorter packing sorts first; equal lengths sort by
 * their bytes. Zero means the two are identical. */
static int
addr_cmp(const struct nc_addr *a, const struct nc_addr *b)
{
    if (a->len != b->len)
        return a->len < b->len ? -1 : 1;
    return memcmp(a->a, b->a, a->len);
}

/* FNV-1a over a run of bytes. The token only has to be the same on both sides
 * and spread out enough to tell sessions apart; it is not a secret and not a
 * MAC, so a small non-cryptographic hash is the right size for the job. */
static uint64_t
fnv64(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

int
nc_reach_session(struct nc_reach *s, const struct nc_addr *me,
                 const struct nc_addr *peer)
{
    const struct nc_addr *lo, *hi;
    uint64_t h;
    int c, i;

    if (me->len == 0 || peer->len == 0)
        return NC_REACH_ERR;
    c = addr_cmp(me, peer);
    if (c == 0)
        return NC_REACH_ERR;        /* identical: both would pick one role */

    /* The lower address is the client, so exactly one of the two connects and
     * the other listens, with no message exchanged to agree on it. */
    if (c < 0) {
        lo = me; hi = peer;
        s->role = NC_REACH_ROLE_CLIENT;
    } else {
        lo = peer; hi = me;
        s->role = NC_REACH_ROLE_SERVER;
    }

    /* Hash the ordered pair, lengths included, so both peers land on the same
     * token no matter which order they pass the two addresses in. */
    h = 1469598103934665603ULL;
    h = fnv64(h, &lo->len, 1);
    h = fnv64(h, lo->a, lo->len);
    h = fnv64(h, &hi->len, 1);
    h = fnv64(h, hi->a, hi->len);
    for (i = 0; i < NC_REACH_TOKEN_LEN; i++)
        s->token[i] = (uint8_t)(h >> (56 - i * 8));

    s->peer = *peer;
    return NC_REACH_OK;
}

long
nc_reach_build(void *out, size_t outlen, const struct nc_reach *s, int kind)
{
    uint8_t *o = out;

    if (outlen < NC_REACH_PACKET_LEN)
        return NC_REACH_ERR;
    if (kind != NC_REACH_PUNCH && kind != NC_REACH_PUNCH_ACK)
        return NC_REACH_ERR;

    /* Magic little-endian, so the bytes read NCR1 in a hex dump, as the beacon
     * writes its own. */
    o[0] = (uint8_t)(NC_REACH_MAGIC & 0xff);
    o[1] = (uint8_t)((NC_REACH_MAGIC >> 8) & 0xff);
    o[2] = (uint8_t)((NC_REACH_MAGIC >> 16) & 0xff);
    o[3] = (uint8_t)((NC_REACH_MAGIC >> 24) & 0xff);
    o[4] = (uint8_t)kind;
    o[5] = 0;
    o[6] = 0;
    o[7] = 0;                       /* reserved, sent zero */
    memcpy(o + 8, s->token, NC_REACH_TOKEN_LEN);
    return NC_REACH_PACKET_LEN;
}

int
nc_reach_parse(const struct nc_reach *s, const void *pkt, size_t len)
{
    const uint8_t *p = pkt;
    uint32_t magic;
    int kind;

    if (len < NC_REACH_PACKET_LEN)
        return NC_REACH_ERR;
    magic = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
          | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    if (magic != NC_REACH_MAGIC)
        return NC_REACH_ERR;
    kind = p[4];
    if (kind != NC_REACH_PUNCH && kind != NC_REACH_PUNCH_ACK)
        return NC_REACH_ERR;
    /* The token tells our session from another's on a shared port. It is not a
     * trust check: a punch that carries it still only proves itself by reaching
     * us and drawing an ack, and the netchan handshake is what the path is
     * trusted for. */
    if (memcmp(p + 8, s->token, NC_REACH_TOKEN_LEN) != 0)
        return NC_REACH_ERR;
    return kind;
}
