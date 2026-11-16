/* test_nc_nat.c : STUN binding and the room-code codec, built and parsed */

#include <stdio.h>
#include <string.h>

#include "nc_nat.h"

/*
 * Neither half has a socket or a peer, so there is nothing to drive: a request
 * is built and a reply is fed back in, and a code is encoded and decoded. What
 * is worth testing is the parsing around that. A STUN reply that is not ours,
 * or is forged, or names an address the way the XOR form hides it. And a room
 * code read the forgiving way a person types it, or mistyped past its check.
 */

static int failures;

static void
check(const char *what, int ok)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        failures++;
}

/* The transaction id used throughout. Its only job in a test is to be the one
 * the parser is told to expect, so a fixed pattern is fine here; a real caller
 * draws it from a random source. */
static const uint8_t TXN[NC_STUN_TXN_LEN] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab,
    0xcd, 0xef, 0x10, 0x32, 0x54, 0x76,
};

#define MAGIC_HI  0x2112
#define MAGIC_LO  0xa442

static void
put_be16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

/*
 * Builds a binding success response for TXN carrying one mapped-address
 * attribute. type is STUN_ATTR_MAPPED_ADDRESS (0x0001, plain) or
 * XOR_MAPPED_ADDRESS (0x0020, obscured under the cookie). Returns its length.
 */
static int
build_response(uint8_t *buf, int attr_type, uint32_t ip, uint16_t port)
{
    uint16_t x_port = port;
    uint32_t x_ip = ip;
    int body = 12;                              /* 4 attr header + 8 value */

    if (attr_type == 0x0020) {
        x_port ^= MAGIC_HI;
        x_ip ^= 0x2112a442u;
    }
    put_be16(buf + 0, 0x0101);                  /* binding success */
    put_be16(buf + 2, (unsigned)body);
    put_be16(buf + 4, MAGIC_HI);
    put_be16(buf + 6, MAGIC_LO);
    memcpy(buf + 8, TXN, NC_STUN_TXN_LEN);
    put_be16(buf + 20, (unsigned)attr_type);
    put_be16(buf + 22, 8);                      /* value length */
    buf[24] = 0;                                /* reserved */
    buf[25] = 0x01;                             /* family IPv4 */
    put_be16(buf + 26, x_port);
    buf[28] = (uint8_t)(x_ip >> 24);
    buf[29] = (uint8_t)(x_ip >> 16);
    buf[30] = (uint8_t)(x_ip >> 8);
    buf[31] = (uint8_t)(x_ip & 0xff);
    return 32;
}

/*
 * The library speaks nc_addr. These adapters read the address back as an
 * integer and port so the checks below stay about behaviour rather than
 * packing; one check in each half tests the nc_addr surface directly.
 */
static int
stun_parse(const uint8_t *resp, size_t n, uint32_t *ip, uint16_t *port)
{
    struct nc_addr a;
    int r = nc_stun_parse_response(resp, n, TXN, &a);

    if (r == NC_NAT_OK)
        nc_addr_get_ipv4(&a, ip, port);
    return r;
}

static long
roomcode_encode(char *out, size_t outlen, uint32_t ip, uint16_t port)
{
    struct nc_addr a;

    nc_addr_set_ipv4(&a, ip, port);
    return nc_roomcode_encode(out, outlen, &a);
}

static int
roomcode_decode(const char *code, uint32_t *ip, uint16_t *port)
{
    struct nc_addr a;
    int r = nc_roomcode_decode(code, &a);

    if (r == NC_NAT_OK)
        nc_addr_get_ipv4(&a, ip, port);
    return r;
}

static void
test_stun(void)
{
    uint8_t req[NC_STUN_REQUEST_LEN];
    uint8_t resp[64];
    uint32_t ip;
    uint16_t port;
    int n;

    /* 1. The request is the fixed shape RFC 5389 describes: the binding
     *    request type, a zero length, the magic cookie, then our id. */
    check("1. a request builds",
          nc_stun_build_request(req, sizeof(req), TXN) == NC_STUN_REQUEST_LEN);
    check("1. it is a binding request",
          req[0] == 0x00 && req[1] == 0x01);
    check("1. with no attributes", req[2] == 0x00 && req[3] == 0x00);
    check("1. and the magic cookie",
          req[4] == 0x21 && req[5] == 0x12 && req[6] == 0xa4 && req[7] == 0x42);
    check("1. carrying our transaction id",
          memcmp(req + 8, TXN, NC_STUN_TXN_LEN) == 0);

    /* 2. A buffer with no room is refused rather than half-written. */
    check("2. a short buffer is refused",
          nc_stun_build_request(req, NC_STUN_REQUEST_LEN - 1, TXN)
              == NC_NAT_ERR);

    /* 3. An XOR-MAPPED-ADDRESS reply yields the address underneath the XOR. */
    n = build_response(resp, 0x0020, 0xcb0071a2u, 47474);
    ip = 0;
    port = 0;
    check("3. an xor reply parses",
          stun_parse(resp, (size_t)n, &ip, &port) == NC_NAT_OK);
    check("3. the address is un-xored", ip == 0xcb0071a2u);
    check("3. and the port", port == 47474);

    /* 4. The legacy MAPPED-ADDRESS form reads straight through. */
    n = build_response(resp, 0x0001, 0x08080808u, 3478);
    ip = 0;
    port = 0;
    check("4. a legacy reply parses",
          stun_parse(resp, (size_t)n, &ip, &port) == NC_NAT_OK);
    check("4. the address is read as-is", ip == 0x08080808u && port == 3478);

    /* 5. A reply to someone else's request is dropped, not believed. This is
     *    the whole anti-spoofing check: change one id byte and it is a
     *    stranger's answer. */
    n = build_response(resp, 0x0020, 0x01020304u, 1234);
    resp[8] ^= 0xff;
    check("5. a mismatched id is rejected",
          stun_parse(resp, (size_t)n, &ip, &port)
              == NC_NAT_ERR);

    /* 6. A wrong magic cookie, a wrong message type, and a short packet are
     *    each refused before anything is read out. */
    n = build_response(resp, 0x0020, 0x01020304u, 1234);
    resp[4] ^= 0x01;
    check("6. a wrong cookie is rejected",
          stun_parse(resp, (size_t)n, &ip, &port)
              == NC_NAT_ERR);
    n = build_response(resp, 0x0020, 0x01020304u, 1234);
    resp[0] = 0x00;
    resp[1] = 0x01;                             /* a request, not a response */
    check("6. a non-success type is rejected",
          stun_parse(resp, (size_t)n, &ip, &port)
              == NC_NAT_ERR);
    n = build_response(resp, 0x0020, 0x01020304u, 1234);
    check("6. a truncated packet is rejected",
          stun_parse(resp, NC_STUN_REQUEST_LEN - 1, &ip, &port)
              == NC_NAT_ERR);

    /* 7. An IPv6 mapped address is not reported: the family byte is not IPv4,
     *    so there is no IPv4 attribute to find and the parse fails. */
    n = build_response(resp, 0x0020, 0x01020304u, 1234);
    resp[25] = 0x02;                            /* family IPv6 */
    check("7. an ipv6-only reply is rejected",
          stun_parse(resp, (size_t)n, &ip, &port)
              == NC_NAT_ERR);

    /* 8. The parsed value is a real IPv4 nc_addr, in the packing nc_udp and a
     *    CONNECT_REDIRECT frame share, so it drops into netchan_connect as-is. */
    {
        struct nc_addr a;

        n = build_response(resp, 0x0020, 0xcb0071a2u, 47474);
        memset(&a, 0, sizeof(a));
        check("8. the result is an nc_addr",
              nc_stun_parse_response(resp, (size_t)n, TXN, &a) == NC_NAT_OK);
        check("8. in the IPv4 packing",
              a.len == 7 && a.a[0] == 4);
    }
}

static void
test_roomcode(void)
{
    char code[NC_ROOMCODE_BUFSIZE];
    uint32_t ip;
    uint16_t port;
    long len;

    /* 1. An address and port survive the round trip, and the code is the
     *    dashed shape a person reads: XXXX-XXXX-XXXX. */
    len = roomcode_encode(code, sizeof(code), 0xcb0071a2u, 47474);
    check("1. a code encodes", len == NC_ROOMCODE_STRLEN);
    check("1. it is dashed in threes",
          strlen(code) == NC_ROOMCODE_STRLEN &&
          code[4] == '-' && code[9] == '-');
    ip = 0;
    port = 0;
    check("1. it decodes",
          roomcode_decode(code, &ip, &port) == NC_NAT_OK);
    check("1. the address survives", ip == 0xcb0071a2u && port == 47474);

    /* 2. The decoder is forgiving the way Crockford's alphabet intends: case
     *    folds, dashes are optional, and O/I/L read as 0/1/1. All three forms
     *    of the same code decode to the same address. */
    {
        char lower[NC_ROOMCODE_BUFSIZE];
        char nodash[NC_ROOMCODE_BUFSIZE];
        uint32_t ip2;
        uint16_t port2;
        size_t i, j;

        roomcode_encode(code, sizeof(code), 0x7f000001u, 8080);
        for (i = 0; code[i]; i++)
            lower[i] = (code[i] >= 'A' && code[i] <= 'Z')
                           ? (char)(code[i] + 32) : code[i];
        lower[i] = '\0';
        for (i = 0, j = 0; code[i]; i++)
            if (code[i] != '-')
                nodash[j++] = code[i];
        nodash[j] = '\0';

        check("2. lower case decodes",
              roomcode_decode(lower, &ip, &port) == NC_NAT_OK);
        check("2. and without dashes",
              roomcode_decode(nodash, &ip2, &port2) == NC_NAT_OK);
        check("2. to the same address",
              ip == 0x7f000001u && port == 8080 &&
              ip2 == ip && port2 == port);
    }

    /* 3. O, I and L fold to 0, 1 and 1. A code written with a letter O where a
     *    zero belongs decodes as if the zero were there. */
    {
        char folded[NC_ROOMCODE_BUFSIZE];
        uint32_t ipo;
        uint16_t porto;
        size_t i;

        roomcode_encode(code, sizeof(code), 0x00000000u, 1);
        ip = 0;
        port = 0;
        roomcode_decode(code, &ip, &port);   /* the reference reading */
        for (i = 0; code[i]; i++)
            folded[i] = code[i] == '0' ? 'O' : (code[i] == '1' ? 'L' : code[i]);
        folded[i] = '\0';
        check("3. O and L fold to 0 and 1",
              roomcode_decode(folded, &ipo, &porto) == NC_NAT_OK &&
              ipo == ip && porto == port);
    }

    /* 4. A zero address is a valid code: the codec carries the bits and leaves
     *    the question of whether an address is worth dialing to the caller. */
    check("4. a zero address encodes",
          roomcode_encode(code, sizeof(code), 0, 0) == NC_ROOMCODE_STRLEN);
    check("4. and decodes back to zero",
          roomcode_decode(code, &ip, &port) == NC_NAT_OK &&
          ip == 0 && port == 0);

    /* 5. A single mistyped character fails the check rather than decoding to
     *    some other address. The check is the point: a typo is "not a code",
     *    never a connection somewhere else. */
    {
        char typo[NC_ROOMCODE_BUFSIZE];
        int rejected = 0, c;

        roomcode_encode(code, sizeof(code), 0xabcdef01u, 25565);
        /* Nudge the first data character to each other symbol; every one must
         * fail the check (a collision would be a weak check digit). */
        for (c = 0; c < 32; c++) {
            memcpy(typo, code, sizeof(code));
            if (typo[0] == "0123456789ABCDEFGHJKMNPQRSTVWXYZ"[c])
                continue;
            typo[0] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"[c];
            if (roomcode_decode(typo, &ip, &port) == NC_NAT_ERR)
                rejected++;
        }
        check("5. every one-character typo is rejected", rejected == 31);
    }

    /* 6. Wrong lengths and out-of-alphabet characters are refused, and a
     *    rejected code leaves the outputs untouched. */
    ip = 0xdeadbeefu;
    port = 0x1234;
    check("6. too short is refused",
          roomcode_decode("ABCD-EFGH", &ip, &port) == NC_NAT_ERR);
    check("6. too long is refused",
          roomcode_decode("ABCD-EFGH-JKMN-PQRS", &ip, &port) == NC_NAT_ERR);
    check("6. a stray character is refused",
          roomcode_decode("ABCD-EFGH-JKM!", &ip, &port) == NC_NAT_ERR);
    check("6. and the outputs are untouched",
          ip == 0xdeadbeefu && port == 0x1234);

    /* 7. A buffer too small to hold the code is refused rather than
     *    truncating it. */
    {
        char tiny[NC_ROOMCODE_BUFSIZE - 1];

        check("7. a small buffer is refused",
              roomcode_encode(tiny, sizeof(tiny), 1, 1) == NC_NAT_ERR);
    }

    /* 8. Encoding takes an nc_addr and refuses one that is not IPv4: an unset
     *    address and an IPv6 one both fail rather than encode nonsense. */
    {
        struct nc_addr unset, v6;

        memset(&unset, 0, sizeof(unset));
        memset(&v6, 0, sizeof(v6));
        v6.len = 19;
        v6.a[0] = 6;
        check("8. an unset address is refused",
              nc_roomcode_encode(code, sizeof(code), &unset) == NC_NAT_ERR);
        check("8. an IPv6 address is refused",
              nc_roomcode_encode(code, sizeof(code), &v6) == NC_NAT_ERR);
    }
}

int
main(void)
{
    test_stun();
    test_roomcode();
    if (failures == 0)
        printf("all checks passed\n");
    return failures != 0;
}
