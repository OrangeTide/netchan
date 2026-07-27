/* test_nc_beacon.c : the discovery packet, built and parsed */

#include <stdio.h>
#include <string.h>

#include "nc_beacon.h"

/*
 * A beacon has no state machine and no peer, so there is nothing to drive: a
 * packet goes out and the same packet comes back in. What is worth testing is
 * everything around that. A browser from last year reading a server from this
 * year. A packet from whoever happens to be on the link, which is to say a
 * hostile one. And the header, since the whole reason a beacon has a magic is
 * to be told apart from traffic that is not a beacon.
 */

static int failures;

static void
check(const char *what, int ok)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        failures++;
}

static struct nc_beacon
sample(void)
{
    struct nc_beacon b;

    memset(&b, 0, sizeof(b));
    b.game = 0x5a4f4d42u;
    b.wire = 1;
    b.major = 2;
    b.minor = 7;
    b.port = 27015;
    b.players = 3;
    b.capacity = 16;
    b.modes = 0x6;
    b.flags = NC_BEACON_F_CREDENTIALS | NC_BEACON_F_REGISTER;
    b.instance = 0xdeadbeefu;
    snprintf(b.name, sizeof(b.name), "Ashen Coast");
    snprintf(b.contact, sizeof(b.contact), "run by Bob, #ashen on irc");
    memset(b.key, 0xa7, sizeof(b.key));
    b.has_key = 1;
    return b;
}

int
main(void)
{
    uint8_t pkt[NC_BEACON_MAX];
    struct nc_beacon in, out;
    long n;

    /* 1. Everything filled in survives the round trip. */
    in = sample();
    n = nc_beacon_build(pkt, sizeof(pkt), &in);
    check("1. a full beacon builds", n > 0 && n <= NC_BEACON_MAX);
    check("1. and stays small", n < 200);
    check("1. it parses", nc_beacon_parse(&out, pkt, (size_t)n) == 0);
    check("1. the numbers survive",
          out.game == in.game && out.wire == in.wire &&
          out.major == in.major && out.minor == in.minor &&
          out.port == in.port && out.players == in.players &&
          out.capacity == in.capacity && out.modes == in.modes &&
          out.flags == in.flags && out.instance == in.instance);
    check("1. the strings survive",
          strcmp(out.name, in.name) == 0 &&
          strcmp(out.contact, in.contact) == 0);
    check("1. the key survives",
          out.has_key && memcmp(out.key, in.key, 32) == 0);

    /* 2. A server that fills in almost nothing sends almost nothing. A zero
     *    field is absent rather than encoded, which is what keeps a beacon
     *    worth broadcasting every few seconds. */
    {
        struct nc_beacon bare;
        long small;

        memset(&bare, 0, sizeof(bare));
        bare.game = 0x5a4f4d42u;
        bare.port = 27015;
        small = nc_beacon_build(pkt, sizeof(pkt), &bare);
        check("2. a sparse beacon is tiny", small > 0 && small < 20);
        check("2. and parses back", nc_beacon_parse(&out, pkt, (size_t)small) == 0);
        check("2. absent fields read as zero",
              out.players == 0 && out.name[0] == '\0' && !out.has_key);
    }

    /* 3. The magic is what tells a beacon from anything else that lands on the
     *    port. Nothing without it decodes, and it reads as itself in a hex
     *    dump, which is the only reason to choose a printable one. */
    {
        uint8_t junk[32];

        in = sample();
        n = nc_beacon_build(pkt, sizeof(pkt), &in);
        check("3. the magic reads as NCB1 on the wire",
              memcmp(pkt, "NCB1", 4) == 0);

        memset(junk, 0x41, sizeof(junk));
        check("3. junk is not a beacon",
              nc_beacon_kind(junk, sizeof(junk)) == NC_BEACON_ERR);
        check("3. and does not parse",
              nc_beacon_parse(&out, junk, sizeof(junk)) == NC_BEACON_ERR);

        in = sample();
        n = nc_beacon_build(pkt, sizeof(pkt), &in);
        pkt[2] ^= 0xff;
        check("3. a corrupted magic is refused",
              nc_beacon_parse(&out, pkt, (size_t)n) == NC_BEACON_ERR);
        pkt[2] ^= 0xff;
        pkt[4] = 99;
        check("3. an unknown kind is refused",
              nc_beacon_kind(pkt, (size_t)n) == NC_BEACON_ERR);
    }

    /* 4. Probes are told apart from announcements without decoding, and the
     *    two never answer for each other. */
    {
        uint32_t game = 0;

        n = nc_beacon_probe(pkt, sizeof(pkt), 0x5a4f4d42u);
        check("4. a probe builds", n > 0);
        check("4. it is a probe",
              nc_beacon_kind(pkt, (size_t)n) == NC_BEACON_PROBE);
        check("4. and names its game",
              nc_beacon_probe_game(pkt, (size_t)n, &game) == 0 &&
              game == 0x5a4f4d42u);
        check("4. a probe is not an announcement",
              nc_beacon_parse(&out, pkt, (size_t)n) == NC_BEACON_ERR);

        in = sample();
        n = nc_beacon_build(pkt, sizeof(pkt), &in);
        check("4. an announcement is not a probe",
              nc_beacon_probe_game(pkt, (size_t)n, &game) == NC_BEACON_ERR);
    }

    /* 5. A browser from last year reads a server from this year. Fields it has
     *    never heard of are skipped, and the ones it knows still arrive. This
     *    is the whole reason the body is tagged. */
    {
        uint8_t future[NC_BEACON_MAX];
        long base;
        int i;

        in = sample();
        base = nc_beacon_build(future, sizeof(future), &in);

        /* Field 20, wire type 4: something a later version added. */
        future[base++] = (uint8_t)((20 << 3) | 4);
        future[base++] = 4;
        future[base++] = 0;
        for (i = 0; i < 4; i++)
            future[base++] = (uint8_t)('x' + i);
        /* And a scalar one, for good measure. */
        future[base++] = (uint8_t)((21 << 3) | 3);
        for (i = 0; i < 8; i++)
            future[base++] = 0x5a;

        check("5. a newer beacon still parses",
              nc_beacon_parse(&out, future, (size_t)base) == 0);
        check("5. the fields we know still arrive",
              out.port == in.port && strcmp(out.name, in.name) == 0 &&
              out.has_key);
    }

    /* 6. Whoever is on the link is not to be trusted, so a malformed packet is
     *    refused rather than half-read. */
    {
        in = sample();
        n = nc_beacon_build(pkt, sizeof(pkt), &in);

        check("6. a truncated body is refused",
              nc_beacon_parse(&out, pkt, (size_t)n - 3) == NC_BEACON_ERR);
        check("6. a header with nothing after it is still a beacon",
              nc_beacon_kind(pkt, 5) == NC_BEACON_ANNOUNCE);
        check("6. and parses as an empty one",
              nc_beacon_parse(&out, pkt, 5) == 0 && out.game == 0);
        check("6. a runt is not a beacon",
              nc_beacon_kind(pkt, 4) == NC_BEACON_ERR);
        check("6. and neither is nothing",
              nc_beacon_kind(pkt, 0) == NC_BEACON_ERR);
    }

    /* 7. A string longer than there is room for is refused, not truncated. A
     *    silently shortened name is a browser showing something the server did
     *    not say. */
    {
        uint8_t hostile[NC_BEACON_MAX];
        int pos = 0, i;
        uint16_t big = NC_BEACON_MAX_NAME + 1;

        memcpy(hostile, pkt, 5);            /* a valid announce header */
        pos = 5;
        hostile[pos++] = (uint8_t)((11 << 3) | 4);      /* F_NAME */
        hostile[pos++] = (uint8_t)(big & 0xff);
        hostile[pos++] = (uint8_t)(big >> 8);
        for (i = 0; i < big; i++)
            hostile[pos++] = 'z';
        check("7. an over-long name is refused",
              nc_beacon_parse(&out, hostile, (size_t)pos) == NC_BEACON_ERR);

        /* And a key that is not a key. */
        pos = 5;
        hostile[pos++] = (uint8_t)((13 << 3) | 4);      /* F_KEY */
        hostile[pos++] = 16;
        hostile[pos++] = 0;
        for (i = 0; i < 16; i++)
            hostile[pos++] = 0x11;
        check("7. a short key is refused",
              nc_beacon_parse(&out, hostile, (size_t)pos) == NC_BEACON_ERR);
    }

    /* 8. A name at exactly the limit is fine, since off-by-one is where a cap
     *    like this actually breaks. */
    {
        struct nc_beacon edge;
        size_t i;

        memset(&edge, 0, sizeof(edge));
        edge.game = 1;
        for (i = 0; i < NC_BEACON_MAX_NAME; i++)
            edge.name[i] = 'n';
        edge.name[NC_BEACON_MAX_NAME] = '\0';
        n = nc_beacon_build(pkt, sizeof(pkt), &edge);
        check("8. a name at the limit builds", n > 0);
        check("8. and comes back whole",
              nc_beacon_parse(&out, pkt, (size_t)n) == 0 &&
              strlen(out.name) == NC_BEACON_MAX_NAME);
    }

    /* 9. A buffer too small to hold the beacon fails rather than writing past
     *    the end. The sanitizers are watching this one. */
    {
        uint8_t tiny[16];

        in = sample();
        check("9. a small buffer is refused",
              nc_beacon_build(tiny, sizeof(tiny), &in) == NC_BEACON_ERR);
        check("9. and one with no room for a header",
              nc_beacon_build(tiny, 2, &in) == NC_BEACON_ERR);
        check("9. probes too",
              nc_beacon_probe(tiny, 3, 1) == NC_BEACON_ERR);
    }

    if (failures == 0)
        printf("all checks passed\n");
    return failures != 0;
}
