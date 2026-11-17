/* test_nc_reach.c : the hole-punch session and punch packet, built and parsed */

#include <stdio.h>
#include <string.h>

#include "nc_reach.h"

/*
 * There is no socket and no peer to drive here, so what is worth testing is the
 * session two peers derive from their two addresses, and the packet one builds
 * and the other reads. The session has to come out the same on both sides from
 * either order of the pair, pick opposite roles, and the punch one peer builds
 * has to parse under the other's session and be told from a stranger's.
 */

static int failures;

static void
check(const char *what, int ok)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        failures++;
}

static struct nc_addr
addr(uint32_t ip, uint16_t port)
{
    struct nc_addr a;
    memset(&a, 0, sizeof(a));
    nc_addr_set_ipv4(&a, ip, port);
    return a;
}

static void
test_session(void)
{
    struct nc_addr a = addr(0x01020304u, 1000);   /* the lower address */
    struct nc_addr b = addr(0x05060708u, 2000);
    struct nc_addr c = addr(0x09090909u, 9999);
    struct nc_reach sa, sb, sa2, unset_s, bad_s;
    struct nc_addr unset;

    memset(&unset, 0, sizeof(unset));

    /* 1. Both peers derive a session from the pair, in either order, and land
     *    on the same token. */
    check("1. a session derives",
          nc_reach_session(&sa, &a, &b) == NC_REACH_OK);
    check("1. and from the other side",
          nc_reach_session(&sb, &b, &a) == NC_REACH_OK);
    check("1. the token is the same on both",
          memcmp(sa.token, sb.token, NC_REACH_TOKEN_LEN) == 0);

    /* 2. Exactly one of the two is the client: the lower address connects, the
     *    higher one listens. */
    check("2. the lower address is the client",
          sa.role == NC_REACH_ROLE_CLIENT);
    check("2. the higher address is the server",
          sb.role == NC_REACH_ROLE_SERVER);

    /* 3. The peer to punch toward is the argument, carried straight. */
    check("3. the punch target is the peer",
          sa.peer.len == b.len && memcmp(sa.peer.a, b.a, b.len) == 0);

    /* 4. The derivation is deterministic: the same pair gives the same token. */
    check("4. the same pair derives again",
          nc_reach_session(&sa2, &a, &b) == NC_REACH_OK);
    check("4. to the same token",
          memcmp(sa.token, sa2.token, NC_REACH_TOKEN_LEN) == 0);

    /* 5. An unset address cannot form a session, and neither can a pair of
     *    identical addresses, where both peers would pick the same role. */
    check("5. an unset address is refused",
          nc_reach_session(&unset_s, &a, &unset) == NC_REACH_ERR);
    check("5. identical addresses are refused",
          nc_reach_session(&bad_s, &a, &a) == NC_REACH_ERR);

    /* 6. A different partner is a different session, so its token differs. */
    {
        struct nc_reach sac;
        check("6. a session with a third peer derives",
              nc_reach_session(&sac, &a, &c) == NC_REACH_OK);
        check("6. its token differs from the first",
              memcmp(sa.token, sac.token, NC_REACH_TOKEN_LEN) != 0);
    }
}

static void
test_packet(void)
{
    struct nc_addr a = addr(0x01020304u, 1000);
    struct nc_addr b = addr(0x05060708u, 2000);
    struct nc_addr c = addr(0x09090909u, 9999);
    struct nc_reach sa, sb, sac;
    uint8_t pkt[32];

    nc_reach_session(&sa, &a, &b);
    nc_reach_session(&sb, &b, &a);
    nc_reach_session(&sac, &a, &c);

    /* 1. A punch is the fixed length, reads NCR1 in a hex dump, names its kind,
     *    and carries the session token. */
    check("1. a punch builds",
          nc_reach_build(pkt, sizeof(pkt), &sa, NC_REACH_PUNCH)
              == NC_REACH_PACKET_LEN);
    check("1. it reads NCR1",
          pkt[0] == 'N' && pkt[1] == 'C' && pkt[2] == 'R' && pkt[3] == '1');
    check("1. it is a punch", pkt[4] == NC_REACH_PUNCH);
    check("1. it carries the token",
          memcmp(pkt + 8, sa.token, NC_REACH_TOKEN_LEN) == 0);

    /* 2. The other peer, holding the same session, reads it as a punch. This is
     *    the round trip that opens a path. */
    check("2. the peer reads it as a punch",
          nc_reach_parse(&sb, pkt, NC_REACH_PACKET_LEN) == NC_REACH_PUNCH);

    /* 3. An ack is built and read the same way. */
    check("3. an ack builds",
          nc_reach_build(pkt, sizeof(pkt), &sb, NC_REACH_PUNCH_ACK)
              == NC_REACH_PACKET_LEN);
    check("3. and reads as an ack",
          nc_reach_parse(&sa, pkt, NC_REACH_PACKET_LEN) == NC_REACH_PUNCH_ACK);

    /* 4. A short buffer and an unknown kind are refused rather than
     *    half-written. */
    check("4. a short buffer is refused",
          nc_reach_build(pkt, NC_REACH_PACKET_LEN - 1, &sa, NC_REACH_PUNCH)
              == NC_REACH_ERR);
    check("4. an unknown kind is refused",
          nc_reach_build(pkt, sizeof(pkt), &sa, 7) == NC_REACH_ERR);

    /* 5. Trailing bytes after the packet are ignored, not a reason to reject. */
    nc_reach_build(pkt, sizeof(pkt), &sa, NC_REACH_PUNCH);
    pkt[NC_REACH_PACKET_LEN] = 0xff;
    check("5. a padded datagram still parses",
          nc_reach_parse(&sb, pkt, NC_REACH_PACKET_LEN + 1) == NC_REACH_PUNCH);

    /* 6. A packet that is not ours is rejected: a wrong magic, a short packet,
     *    an unknown kind, and a token from a different session all fail. */
    nc_reach_build(pkt, sizeof(pkt), &sa, NC_REACH_PUNCH);
    pkt[0] ^= 0xff;
    check("6. a wrong magic is rejected",
          nc_reach_parse(&sb, pkt, NC_REACH_PACKET_LEN) == NC_REACH_ERR);

    nc_reach_build(pkt, sizeof(pkt), &sa, NC_REACH_PUNCH);
    check("6. a short packet is rejected",
          nc_reach_parse(&sb, pkt, NC_REACH_PACKET_LEN - 1) == NC_REACH_ERR);

    nc_reach_build(pkt, sizeof(pkt), &sa, NC_REACH_PUNCH);
    pkt[4] = 9;
    check("6. an unknown kind is rejected",
          nc_reach_parse(&sb, pkt, NC_REACH_PACKET_LEN) == NC_REACH_ERR);

    /* A punch for the A-C session carries the A-C token, which the A-B peer
     *    does not hold, so it is not read as ours. */
    nc_reach_build(pkt, sizeof(pkt), &sac, NC_REACH_PUNCH);
    check("6. a punch from another session is rejected",
          nc_reach_parse(&sb, pkt, NC_REACH_PACKET_LEN) == NC_REACH_ERR);
}

int
main(void)
{
    test_session();
    test_packet();
    if (failures == 0)
        printf("all checks passed\n");
    return failures != 0;
}
