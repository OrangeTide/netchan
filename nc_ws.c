/* nc_ws.c : dependency-free WebSocket framing and handshake for netchan */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "nc_ws.h"

#include <string.h>
#include <stdio.h>

/****************************************************************
 * SHA-1 (RFC 3174) -- needed only for the handshake accept key
 ****************************************************************/

struct sha1 {
    uint32_t h[5];
    uint64_t len;          /* message length in bytes */
    uint8_t  buf[64];
    size_t   n;            /* bytes buffered */
};

static uint32_t
rol32(uint32_t v, int c)
{
    return (v << c) | (v >> (32 - c));
}

static void
sha1_block(struct sha1 *s, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8f1bbcdc; }
        else             { f = b ^ c ^ d;                     k = 0xca62c1d6; }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void
sha1_init(struct sha1 *s)
{
    s->h[0] = 0x67452301; s->h[1] = 0xefcdab89; s->h[2] = 0x98badcfe;
    s->h[3] = 0x10325476; s->h[4] = 0xc3d2e1f0;
    s->len = 0;
    s->n = 0;
}

static void
sha1_update(struct sha1 *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    s->len += len;
    while (len > 0) {
        size_t take = 64 - s->n;
        if (take > len) take = len;
        memcpy(s->buf + s->n, p, take);
        s->n += take; p += take; len -= take;
        if (s->n == 64) { sha1_block(s, s->buf); s->n = 0; }
    }
}

static void
sha1_final(struct sha1 *s, uint8_t out[20])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    uint8_t zero = 0;
    while (s->n != 56)
        sha1_update(s, &zero, 1);
    uint8_t lenbe[8];
    for (int i = 0; i < 8; i++)
        lenbe[i] = (uint8_t)(bits >> (56 - i * 8));
    sha1_update(s, lenbe, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(s->h[i]);
    }
}

/****************************************************************
 * base64 encode
 ****************************************************************/

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Encode len bytes into out (NUL-terminated). out must hold 4*ceil(len/3)+1. */
static void
base64(char *out, const uint8_t *in, size_t len)
{
    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = B64[v & 63];
        i += 3;
    }
    if (len - i == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (len - i == 2) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8;
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = '\0';
}

/****************************************************************
 * handshake helpers
 ****************************************************************/

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* Compute the Sec-WebSocket-Accept value for a client key string. */
static void
accept_key(char out[32], const char *client_key, size_t key_len)
{
    struct sha1 s;
    uint8_t digest[20];
    sha1_init(&s);
    sha1_update(&s, client_key, key_len);
    sha1_update(&s, WS_GUID, sizeof(WS_GUID) - 1);
    sha1_final(&s, digest);
    base64(out, digest, 20);      /* 28 chars + NUL */
}

/* Case-insensitive search for a header line, returning a pointer to the value
 * (past the colon and leading spaces) and its length via *val_len. Returns
 * NULL if the header is absent within req[0..len). */
static const char *
find_header(const char *req, size_t len, const char *name, size_t *val_len)
{
    size_t nlen = strlen(name);
    for (size_t i = 0; i + nlen < len; i++) {
        /* header names start at the beginning of a line */
        if (i != 0 && !(req[i - 1] == '\n'))
            continue;
        size_t j = 0;
        while (j < nlen) {
            char a = req[i + j], b = name[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            j++;
        }
        if (j != nlen || req[i + nlen] != ':')
            continue;
        const char *v = req + i + nlen + 1;
        const char *end = req + len;
        while (v < end && (*v == ' ' || *v == '\t'))
            v++;
        const char *e = v;
        while (e < end && *e != '\r' && *e != '\n')
            e++;
        *val_len = (size_t)(e - v);
        return v;
    }
    return NULL;
}

int
nc_ws_accept(const char *req, size_t req_len, char *resp, size_t resp_cap)
{
    /* Need the full header block before we can answer. */
    int have_end = 0;
    for (size_t i = 0; i + 3 < req_len; i++)
        if (req[i] == '\r' && req[i + 1] == '\n' &&
            req[i + 2] == '\r' && req[i + 3] == '\n') { have_end = 1; break; }
    if (!have_end)
        return 0;

    size_t up_len, key_len;
    const char *up = find_header(req, req_len, "Upgrade", &up_len);
    const char *key = find_header(req, req_len, "Sec-WebSocket-Key", &key_len);
    if (!up || !key || key_len == 0)
        return -1;
    /* Upgrade value must contain "websocket" (case-insensitive). */
    int ws = 0;
    for (size_t i = 0; up_len >= 9 && i + 9 <= up_len; i++) {
        if ((up[i] | 32) == 'w' && (up[i + 1] | 32) == 'e' &&
            (up[i + 2] | 32) == 'b' && (up[i + 3] | 32) == 's' &&
            (up[i + 4] | 32) == 'o' && (up[i + 5] | 32) == 'c' &&
            (up[i + 6] | 32) == 'k' && (up[i + 7] | 32) == 'e' &&
            (up[i + 8] | 32) == 't') { ws = 1; break; }
    }
    if (!ws)
        return -1;

    char akey[32];
    accept_key(akey, key, key_len);
    int n = snprintf(resp, resp_cap,
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", akey);
    if (n < 0 || (size_t)n >= resp_cap)
        return -1;
    return n;
}

size_t
nc_ws_client_request(char *buf, size_t cap, const char *host,
                     const char *path, const uint8_t key16[16], char expect[32])
{
    char key64[32];
    base64(key64, key16, 16);     /* 24 chars + NUL */
    accept_key(expect, key64, strlen(key64));
    int n = snprintf(buf, cap,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n", path, host, key64);
    if (n < 0 || (size_t)n >= cap)
        return 0;
    return (size_t)n;
}

int
nc_ws_client_verify(const char *resp, size_t len, const char *expect)
{
    int have_end = 0;
    for (size_t i = 0; i + 3 < len; i++)
        if (resp[i] == '\r' && resp[i + 1] == '\n' &&
            resp[i + 2] == '\r' && resp[i + 3] == '\n') { have_end = 1; break; }
    if (!have_end)
        return 0;
    /* Status line must be 101. */
    if (len < 12 || memcmp(resp, "HTTP/1.1 101", 12) != 0)
        return -1;
    size_t alen;
    const char *acc = find_header(resp, len, "Sec-WebSocket-Accept", &alen);
    if (!acc || alen != strlen(expect) || memcmp(acc, expect, alen) != 0)
        return -1;
    return 1;
}

/****************************************************************
 * frame codec
 ****************************************************************/

long
nc_ws_frame_parse(uint8_t *buf, size_t len, struct nc_ws_frame *f)
{
    if (len < 2)
        return 0;
    int masked = (buf[1] & 0x80) != 0;
    uint64_t plen = buf[1] & 0x7f;
    size_t hdr = 2;

    if (plen == 126) {
        if (len < 4) return 0;
        plen = (uint64_t)buf[2] << 8 | buf[3];
        hdr = 4;
    } else if (plen == 127) {
        if (len < 10) return 0;
        plen = 0;
        for (int i = 0; i < 8; i++)
            plen = plen << 8 | buf[2 + i];
        hdr = 10;
    }
    if (masked)
        hdr += 4;
    if (len < hdr + plen)
        return 0;                 /* frame not fully arrived yet */

    uint8_t *payload = buf + hdr;
    if (masked) {
        const uint8_t *mk = buf + hdr - 4;
        for (uint64_t i = 0; i < plen; i++)
            payload[i] ^= mk[i & 3];
    }
    f->opcode = buf[0] & 0x0f;
    f->fin = (buf[0] & 0x80) != 0;
    f->payload = payload;
    f->payload_len = (size_t)plen;
    return (long)(hdr + plen);
}

size_t
nc_ws_frame_build(uint8_t *out, size_t cap, int opcode,
                  const void *payload, size_t payload_len, const uint8_t mask4[4])
{
    size_t hdr = 2;
    if (payload_len > 0xffff)
        hdr += 8;
    else if (payload_len >= 126)
        hdr += 2;
    if (mask4)
        hdr += 4;
    if (cap < hdr + payload_len)
        return 0;

    out[0] = (uint8_t)(0x80 | (opcode & 0x0f));   /* FIN + opcode */
    uint8_t mbit = mask4 ? 0x80 : 0;
    size_t o;
    if (payload_len > 0xffff) {
        out[1] = mbit | 127;
        for (int i = 0; i < 8; i++)
            out[2 + i] = (uint8_t)(payload_len >> (56 - i * 8));
        o = 10;
    } else if (payload_len >= 126) {
        out[1] = mbit | 126;
        out[2] = (uint8_t)(payload_len >> 8);
        out[3] = (uint8_t)payload_len;
        o = 4;
    } else {
        out[1] = mbit | (uint8_t)payload_len;
        o = 2;
    }

    const uint8_t *src = payload;
    if (mask4) {
        memcpy(out + o, mask4, 4);
        o += 4;
        for (size_t i = 0; i < payload_len; i++)
            out[o + i] = src[i] ^ mask4[i & 3];
    } else {
        memcpy(out + o, src, payload_len);
    }
    return o + payload_len;
}
