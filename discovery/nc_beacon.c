/* nc_beacon.c : building and parsing the discovery packet */

#include "nc_beacon.h"
#include "microser.h"

#include <string.h>

/*
 * The packet is a 4-byte magic, a kind byte, and a tagged body. The magic is
 * what stops a beacon that arrives on the wrong port from being read as
 * something else, and the kind byte is outside the body so a server can sort
 * probes from announcements without decoding either.
 */

#define HDR 5

/* Field numbers. Once shipped these are fixed forever: a reader skips a tag it
 * does not know, which only helps if a tag never changes meaning. */
#define F_GAME      1
#define F_WIRE      2
#define F_MAJOR     3
#define F_MINOR     4
#define F_PORT      5
#define F_PLAYERS   6
#define F_CAPACITY  7
#define F_MODES     8
#define F_FLAGS     9
#define F_INSTANCE 10
#define F_NAME     11
#define F_CONTACT  12
#define F_KEY      13

/* strnlen is POSIX rather than C11, and this library builds for wasm and for
 * Windows as readily as for a desktop unix. Four lines is cheaper than a
 * feature test. */
static size_t
bounded_len(const char *s, size_t cap)
{
    size_t n = 0;

    while (n < cap && s[n] != '\0')
        n++;
    return n;
}

static void
put_header(uint8_t *out, uint8_t kind)
{
    out[0] = (uint8_t)(NC_BEACON_MAGIC & 0xff);
    out[1] = (uint8_t)((NC_BEACON_MAGIC >> 8) & 0xff);
    out[2] = (uint8_t)((NC_BEACON_MAGIC >> 16) & 0xff);
    out[3] = (uint8_t)((NC_BEACON_MAGIC >> 24) & 0xff);
    out[4] = kind;
}

int
nc_beacon_kind(const void *pkt, size_t len)
{
    const uint8_t *p = pkt;
    uint32_t magic;

    if (len < HDR || len > NC_BEACON_MAX)
        return NC_BEACON_ERR;
    magic = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    if (magic != NC_BEACON_MAGIC)
        return NC_BEACON_ERR;
    if (p[4] != NC_BEACON_ANNOUNCE && p[4] != NC_BEACON_PROBE)
        return NC_BEACON_ERR;
    return p[4];
}

long
nc_beacon_build(void *out, size_t outlen, const struct nc_beacon *b)
{
    uint8_t *o = out;
    int cap = (int)(outlen < NC_BEACON_MAX ? outlen : NC_BEACON_MAX);
    int pos = HDR;
    size_t n;

    if (cap < HDR)
        return NC_BEACON_ERR;
    put_header(o, NC_BEACON_ANNOUNCE);

    /* A zero field is simply absent, so a server that fills in three of these
     * sends three of these. */
    if (b->game)
        pos = ms_write_tag_u32(o, pos, cap, F_GAME, b->game);
    if (pos >= 0 && b->wire)
        pos = ms_write_tag_u16(o, pos, cap, F_WIRE, b->wire);
    if (pos >= 0 && b->major)
        pos = ms_write_tag_u16(o, pos, cap, F_MAJOR, b->major);
    if (pos >= 0 && b->minor)
        pos = ms_write_tag_u16(o, pos, cap, F_MINOR, b->minor);
    if (pos >= 0 && b->port)
        pos = ms_write_tag_u16(o, pos, cap, F_PORT, b->port);
    if (pos >= 0 && b->players)
        pos = ms_write_tag_u16(o, pos, cap, F_PLAYERS, b->players);
    if (pos >= 0 && b->capacity)
        pos = ms_write_tag_u16(o, pos, cap, F_CAPACITY, b->capacity);
    if (pos >= 0 && b->modes)
        pos = ms_write_tag_u32(o, pos, cap, F_MODES, b->modes);
    if (pos >= 0 && b->flags)
        pos = ms_write_tag_u32(o, pos, cap, F_FLAGS, b->flags);
    if (pos >= 0 && b->instance)
        pos = ms_write_tag_u32(o, pos, cap, F_INSTANCE, b->instance);

    n = bounded_len(b->name, sizeof(b->name));
    if (pos >= 0 && n > 0 && n <= NC_BEACON_MAX_NAME)
        pos = ms_write_tag_bytes(o, pos, cap, F_NAME, b->name, (uint16_t)n);

    n = bounded_len(b->contact, sizeof(b->contact));
    if (pos >= 0 && n > 0 && n <= NC_BEACON_MAX_CONTACT)
        pos = ms_write_tag_bytes(o, pos, cap, F_CONTACT, b->contact,
                                 (uint16_t)n);

    if (pos >= 0 && b->has_key)
        pos = ms_write_tag_bytes(o, pos, cap, F_KEY, b->key, 32);

    return pos < 0 ? NC_BEACON_ERR : pos;
}

long
nc_beacon_probe(void *out, size_t outlen, uint32_t game)
{
    uint8_t *o = out;
    int cap = (int)(outlen < NC_BEACON_MAX ? outlen : NC_BEACON_MAX);
    int pos;

    if (cap < HDR)
        return NC_BEACON_ERR;
    put_header(o, NC_BEACON_PROBE);
    pos = ms_write_tag_u32(o, HDR, cap, F_GAME, game);
    return pos < 0 ? NC_BEACON_ERR : pos;
}

/* Copy a string field out of the packet, refusing one too long to hold. */
static int
take_str(char *dst, size_t dstlen, const uint8_t *src, uint16_t n)
{
    if ((size_t)n + 1 > dstlen)
        return NC_BEACON_ERR;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return NC_BEACON_OK;
}

int
nc_beacon_parse(struct nc_beacon *b, const void *pkt, size_t len)
{
    const uint8_t *p = pkt;
    int pos = HDR;
    int end = (int)len;

    memset(b, 0, sizeof(*b));
    if (nc_beacon_kind(pkt, len) != NC_BEACON_ANNOUNCE)
        return NC_BEACON_ERR;

    while (pos < end) {
        uint8_t tag = p[pos++];
        uint8_t field = (uint8_t)(tag >> 3);
        uint8_t wire = (uint8_t)(tag & 7);
        const uint8_t *data;
        uint16_t dlen;

        if (wire == MS_WIRE_BYTES) {
            pos = ms_read_bytes(p, pos, end, &data, 0xffff, &dlen);
            if (pos < 0)
                return NC_BEACON_ERR;
            switch (field) {
            case F_NAME:
                if (take_str(b->name, sizeof(b->name), data, dlen) != 0)
                    return NC_BEACON_ERR;
                break;
            case F_CONTACT:
                if (take_str(b->contact, sizeof(b->contact), data, dlen) != 0)
                    return NC_BEACON_ERR;
                break;
            case F_KEY:
                if (dlen != 32)
                    return NC_BEACON_ERR;
                memcpy(b->key, data, 32);
                b->has_key = 1;
                break;
            default:
                break;              /* a field from a later version */
            }
            continue;
        }

        if (wire == MS_WIRE_16) {
            uint16_t v;

            pos = ms_read_u16(p, pos, end, &v);
            if (pos < 0)
                return NC_BEACON_ERR;
            switch (field) {
            case F_WIRE:     b->wire = v; break;
            case F_MAJOR:    b->major = v; break;
            case F_MINOR:    b->minor = v; break;
            case F_PORT:     b->port = v; break;
            case F_PLAYERS:  b->players = v; break;
            case F_CAPACITY: b->capacity = v; break;
            default: break;
            }
            continue;
        }

        if (wire == MS_WIRE_32) {
            uint32_t v;

            pos = ms_read_u32(p, pos, end, &v);
            if (pos < 0)
                return NC_BEACON_ERR;
            switch (field) {
            case F_GAME:     b->game = v; break;
            case F_MODES:    b->modes = v; break;
            case F_FLAGS:    b->flags = v; break;
            case F_INSTANCE: b->instance = v; break;
            default: break;
            }
            continue;
        }

        pos = ms_skip(p, pos, end, wire);
        if (pos < 0)
            return NC_BEACON_ERR;
    }
    return NC_BEACON_OK;
}

int
nc_beacon_probe_game(const void *pkt, size_t len, uint32_t *game)
{
    const uint8_t *p = pkt;
    int pos = HDR;
    int end = (int)len;

    if (nc_beacon_kind(pkt, len) != NC_BEACON_PROBE)
        return NC_BEACON_ERR;

    *game = 0;
    while (pos < end) {
        uint8_t tag = p[pos++];
        uint8_t field = (uint8_t)(tag >> 3);
        uint8_t wire = (uint8_t)(tag & 7);

        if (field == F_GAME && wire == MS_WIRE_32) {
            pos = ms_read_u32(p, pos, end, game);
        } else {
            pos = ms_skip(p, pos, end, wire);
        }
        if (pos < 0)
            return NC_BEACON_ERR;
    }
    return NC_BEACON_OK;
}
