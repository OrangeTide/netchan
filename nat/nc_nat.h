/*
 * nc_nat.h : learning your public address, and naming it so a friend can type it
 *
 * Made by a machine.
 *
 * Two small helpers for the problem netchan does not otherwise touch: two
 * people behind home routers who want to play over the internet. Neither is
 * the protocol core, and neither opens a socket or runs a timer, for the same
 * reason the core and nc_beacon never do. They are a sibling of netchan, not a
 * layer of it. See docs/content/nat/ for the longer argument and for why real
 * NAT traversal is a separate, larger thing.
 *
 *   nc_stun_*      builds a STUN binding request and parses the reply, so the
 *                  application can ask a public server "what address did my
 *                  packet come from?" and learn the IPv4 and port a friend has
 *                  to reach. This works when the router will forward a port by
 *                  hand; it does not open one.
 *
 *   nc_roomcode_*  turns that address into a short string a person can read
 *                  over voice chat and type back, with a check against typos,
 *                  and back again. No server is involved: a room code is just
 *                  an address written short.
 *
 * Both speak struct nc_addr, the address type the rest of netchan already
 * trades in, so an address STUN learns or a room code decodes drops straight
 * into netchan_connect, into a CONNECT_REDIRECT frame, or into a connection's
 * peer address with no translation. The only dependency is src/nc_addr.h, which
 * is header-only, so this file still links against nothing. Today only IPv4
 * addresses are carried; nc_addr has room for IPv6, so a wider room code later
 * is a change to the code format, not to these signatures.
 *
 * WHAT THIS IS NOT
 *
 * It is not NAT traversal. A STUN reply tells you your public address; it does
 * not make a closed router accept an unsolicited packet on it. The address is
 * useful when a port is already forwarded, or behind a NAT friendly enough that
 * the outgoing STUN packet leaves the door ajar. Punching through a router that
 * will not cooperate needs a rendezvous server and both peers sending at once,
 * which is a larger thing built on the CONNECT_REDIRECT frame.
 *
 * A room code is an address, not a secret. Anyone it is given to, or who reads
 * it off a stream, learns where you are. Treat it like your address.
 */

#ifndef NC_NAT_H
#define NC_NAT_H

#include <stdint.h>
#include <stddef.h>

#include "nc_addr.h"

#define NC_NAT_OK  (0)
#define NC_NAT_ERR (-1)

/* ------------------------------------------------------------------ STUN -- */

/*
 * STUN binding, the subset of RFC 5389 that answers one question. The
 * application owns the socket, the three-tries-a-second-apart retransmission,
 * and the timeout, exactly as it owns a beacon's announce timer. This file
 * builds one packet and parses one packet. That is all.
 *
 * THE TRANSACTION ID MUST BE UNPREDICTABLE
 *
 * The reply is matched to the request by a 96-bit transaction id the caller
 * chooses. It is the only thing that keeps an off-path attacker from feeding
 * you a forged "your address is ..." answer, so it has to come from a real
 * random source, not a counter or a clock. netchan ships one in nc_crypto; a
 * caller that vendors this file alone supplies its own.
 */

#define NC_STUN_REQUEST_LEN  20      /* a binding request is exactly this long */
#define NC_STUN_TXN_LEN      12      /* 96 bits of transaction id */
#define NC_STUN_RESP_MAX    576      /* a reply never needs more buffer */

/**
 * Build a binding request carrying the given transaction id. Returns its
 * length (NC_STUN_REQUEST_LEN), or NC_NAT_ERR if it does not fit in outlen.
 * The caller fills txn with NC_STUN_TXN_LEN unpredictable bytes first and keeps
 * them to match the reply.
 */
long
nc_stun_build_request(void *out, size_t outlen,
                      const uint8_t txn[NC_STUN_TXN_LEN]);

/**
 * Parse a datagram as the reply to the request built with txn. On a binding
 * success response that matches txn and carries an IPv4 mapped address
 * (XOR-MAPPED-ADDRESS, or the legacy MAPPED-ADDRESS), fills *out with that
 * public address in the nc_addr IPv4 packing and returns NC_NAT_OK. The result
 * is ready for netchan_connect or a CONNECT_REDIRECT frame.
 *
 * Returns NC_NAT_ERR for anything else: a short packet, a wrong message type, a
 * transaction id that is not the one we sent (so a stray or forged reply is
 * dropped, not believed), or a reply that only names an IPv6 address. On
 * NC_NAT_ERR *out is untouched. Rejecting a mismatched id is the whole of the
 * anti-spoofing check, which is why the id had to be random.
 *
 * IPv6 is deliberately not reported here. A v6 host that can be reached directly
 * does not need STUN to learn its address, and the room code below is v4-only
 * regardless. A future wider code would change both together.
 */
int
nc_stun_parse_response(const void *pkt, size_t len,
                       const uint8_t txn[NC_STUN_TXN_LEN],
                       struct nc_addr *out);

/* -------------------------------------------------------------- room code -- */

/*
 * A room code is 48 bits of address (32 of IPv4, 16 of port) plus a 12-bit
 * check, written as 12 characters of Crockford's base 32 in three groups of
 * four: for example FACE-8008-5309. The check is not security; it catches the
 * digit a person misheard or mistyped before a packet is ever sent, so a typo
 * is "that is not a room code", not a connection to a stranger.
 *
 * Decoding is forgiving the way Crockford's alphabet intends: case is ignored,
 * the dashes are optional, and the letters O, I and L read as 0, 1 and 1. A
 * caller tries a code before it tries a plain host:port, so a mistyped code is
 * reported as a bad code rather than fed to the resolver.
 */

#define NC_ROOMCODE_LEN       12     /* base32 characters, no separators */
#define NC_ROOMCODE_STRLEN    14     /* with the two dashes: XXXX-XXXX-XXXX */
#define NC_ROOMCODE_BUFSIZE   15     /* NC_ROOMCODE_STRLEN + the terminator */

/**
 * Encode an IPv4 nc_addr as a dashed room code. Writes NC_ROOMCODE_STRLEN
 * characters and a terminator. Returns the string length (NC_ROOMCODE_STRLEN),
 * or NC_NAT_ERR if outlen is below NC_ROOMCODE_BUFSIZE or addr is not an IPv4
 * address. A zero address encodes like any other; it is the caller's business
 * whether an address is worth handing out.
 */
long
nc_roomcode_encode(char *out, size_t outlen, const struct nc_addr *addr);

/**
 * Decode a room code into an IPv4 nc_addr. Accepts the code with or without
 * dashes, in any case, with O/I/L for 0/1/1. Returns NC_NAT_OK, or NC_NAT_ERR if
 * the code is the wrong length, holds a character outside the alphabet, or fails
 * its check digits. On NC_NAT_ERR *out is untouched.
 */
int
nc_roomcode_decode(const char *code, struct nc_addr *out);

#endif /* NC_NAT_H */
