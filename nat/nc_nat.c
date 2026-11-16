/*
 * nc_nat.c : STUN binding and the room-code codec (see nat/nc_nat.h)
 *
 * Made by a machine.
 *
 * Two independent helpers that share only a header and a currency: an IPv4
 * address and a port in host byte order. Neither opens a socket or runs a
 * timer. The STUN half builds one packet and parses one; the room-code half is
 * a pure codec.
 */

#include "nc_nat.h"

#include <string.h>

/* ------------------------------------------------------------------ STUN -- */

/*
 * RFC 5389. A request is a fixed 20-byte header: a 16-bit message type, a
 * 16-bit length (zero, this request has no attributes), a 32-bit magic cookie,
 * and the 96-bit transaction id. A success response repeats the cookie and id
 * and carries the mapped address as a tagged attribute.
 */

#define STUN_BINDING_REQUEST   0x0001
#define STUN_BINDING_SUCCESS   0x0101
#define STUN_MAGIC_COOKIE      0x2112a442u

#define STUN_ATTR_MAPPED_ADDRESS      0x0001
#define STUN_ATTR_XOR_MAPPED_ADDRESS  0x0020
#define STUN_FAMILY_IPV4              0x01

static void
put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

static uint16_t
get_be16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t
get_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16
         | (uint32_t)p[2] << 8 | p[3];
}

long
nc_stun_build_request(void *out, size_t outlen,
                      const uint8_t txn[NC_STUN_TXN_LEN])
{
    uint8_t *p = out;

    if (outlen < NC_STUN_REQUEST_LEN)
        return NC_NAT_ERR;
    put_be16(p + 0, STUN_BINDING_REQUEST);
    put_be16(p + 2, 0);                         /* no attributes */
    put_be16(p + 4, (uint16_t)(STUN_MAGIC_COOKIE >> 16));
    put_be16(p + 6, (uint16_t)(STUN_MAGIC_COOKIE & 0xffff));
    memcpy(p + 8, txn, NC_STUN_TXN_LEN);
    return NC_STUN_REQUEST_LEN;
}

/*
 * Reads one mapped-address attribute into *ip, *port. v points at the
 * attribute value and len is its declared length. Returns NC_NAT_OK for an
 * IPv4 address we could read, NC_NAT_ERR otherwise. XOR-MAPPED-ADDRESS hides
 * the address under the cookie so a dumb NAT cannot rewrite it in flight; the
 * legacy form does not.
 */
static int
read_mapped_ipv4(int type, const uint8_t *v, int len,
                 uint32_t *ip, uint16_t *port)
{
    uint16_t p;
    uint32_t a;

    if (len < 8 || v[1] != STUN_FAMILY_IPV4)
        return NC_NAT_ERR;
    p = get_be16(v + 2);
    a = get_be32(v + 4);
    if (type == STUN_ATTR_XOR_MAPPED_ADDRESS) {
        p ^= (uint16_t)(STUN_MAGIC_COOKIE >> 16);
        a ^= STUN_MAGIC_COOKIE;
    }
    *ip = a;
    *port = p;
    return NC_NAT_OK;
}

int
nc_stun_parse_response(const void *pkt, size_t len,
                       const uint8_t txn[NC_STUN_TXN_LEN],
                       uint32_t *ipv4_out, uint16_t *port_out)
{
    const uint8_t *buf = pkt;
    int body, end, i;
    int found = 0;
    uint32_t ip = 0;
    uint16_t port = 0;

    /* A binding success response whose cookie and id are the ones we sent.
     * The id match is the whole anti-spoofing check: a reply to someone
     * else's request, or a forged one from off the path, fails here. */
    if (len < NC_STUN_REQUEST_LEN)
        return NC_NAT_ERR;
    if (get_be16(buf + 0) != STUN_BINDING_SUCCESS)
        return NC_NAT_ERR;
    if (get_be32(buf + 4) != STUN_MAGIC_COOKIE)
        return NC_NAT_ERR;
    if (memcmp(buf + 8, txn, NC_STUN_TXN_LEN) != 0)
        return NC_NAT_ERR;

    /* Attributes run from byte 20 for the length the header declares, never
     * past the bytes actually received. */
    body = 20 + get_be16(buf + 2);
    end = body < (int)len ? body : (int)len;
    for (i = 20; i + 4 <= end;) {
        int type = get_be16(buf + i);
        int alen = get_be16(buf + i + 2);
        const uint8_t *v = buf + i + 4;

        if (i + 4 + alen > end)
            break;
        if (type == STUN_ATTR_XOR_MAPPED_ADDRESS
            || type == STUN_ATTR_MAPPED_ADDRESS) {
            if (read_mapped_ipv4(type, v, alen, &ip, &port) == NC_NAT_OK) {
                found = 1;
                /* Prefer the XOR form and stop; otherwise keep reading in
                 * case an XOR attribute follows the legacy one. */
                if (type == STUN_ATTR_XOR_MAPPED_ADDRESS)
                    break;
            }
        }
        i += 4 + ((alen + 3) & ~3);             /* attributes are 4-byte aligned */
    }
    if (!found)
        return NC_NAT_ERR;
    *ipv4_out = ip;
    *port_out = port;
    return NC_NAT_OK;
}

/* -------------------------------------------------------------- room code -- */

/*
 * 48 bits of address and port become 60 bits once a 12-bit check is appended,
 * which is exactly 12 base-32 characters. The alphabet is Crockford's: it has
 * no I, L, O or U, so no two symbols look or sound alike, and the decoder folds
 * the mistakes a person still makes (O for 0, I or L for 1) back in.
 */

static const char ROOM_ALPHABET[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/* A 12-bit check over the 48-bit payload. FNV-1a folded down: not a CRC and not
 * a MAC, just enough to reject a mistyped character before a packet is sent. */
static uint32_t
room_check(uint64_t v)
{
    uint32_t h = 2166136261u;
    int i;

    for (i = 0; i < 6; i++)
        h = (h ^ (uint8_t)(v >> (i * 8))) * 16777619u;
    return (h ^ h >> 12 ^ h >> 24) & 0xfff;
}

long
nc_roomcode_encode(char *out, size_t outlen, uint32_t ipv4, uint16_t port)
{
    uint64_t v;
    int i, n = 0;

    if (outlen < NC_ROOMCODE_BUFSIZE)
        return NC_NAT_ERR;
    v = (uint64_t)ipv4 << 16 | port;
    v = v << 12 | room_check(v);
    for (i = 11; i >= 0; i--) {
        out[n++] = ROOM_ALPHABET[(v >> (i * 5)) & 31];
        if (i == 8 || i == 4)
            out[n++] = '-';
    }
    out[n] = '\0';
    return n;                                   /* NC_ROOMCODE_STRLEN */
}

/* The index of c in the alphabet, after folding case and the look-alikes, or
 * -1 for a character the alphabet does not hold. */
static int
room_symbol(char c)
{
    const char *t;

    if (c >= 'a' && c <= 'z')
        c = (char)(c - 32);
    if (c == 'O')
        c = '0';
    if (c == 'I' || c == 'L')
        c = '1';
    t = memchr(ROOM_ALPHABET, c, sizeof(ROOM_ALPHABET) - 1);
    return t ? (int)(t - ROOM_ALPHABET) : -1;
}

int
nc_roomcode_decode(const char *code, uint32_t *ipv4_out, uint16_t *port_out)
{
    uint64_t v = 0;
    const char *s;
    int n = 0;

    for (s = code; *s != '\0'; s++) {
        int sym;

        if (*s == '-' || *s == ' ')
            continue;                           /* separators are optional */
        sym = room_symbol(*s);
        if (sym < 0 || ++n > NC_ROOMCODE_LEN)
            return NC_NAT_ERR;
        v = v << 5 | (uint64_t)sym;
    }
    if (n != NC_ROOMCODE_LEN || room_check(v >> 12) != (v & 0xfff))
        return NC_NAT_ERR;
    *ipv4_out = (uint32_t)(v >> 28);
    *port_out = (uint16_t)(v >> 12);
    return NC_NAT_OK;
}
