/*
 * nc_reach.h : punching a path between two peers that both sit behind a NAT
 *
 * Made by a machine.
 *
 * The [nat] helpers learn your public address and write it short. This file is
 * the next piece: two peers who both sit behind a router that will not forward
 * a port, and who want a path between them anyway. Each has learned its own
 * public address with STUN, and a rendezvous has told each the other's. What is
 * left is to open the two NAT bindings at once so a packet can cross, which is
 * what punching is.
 *
 * It is a sibling of netchan, not a layer of it, for the same reason nc_beacon
 * and nc_nat are. It opens no socket, runs no timer, and picks no address. The
 * application owns its event loop and its clock, fires the punches on a cadence
 * it chooses, and hands the opened path to netchan_connect. This file derives a
 * session from the two addresses and builds and parses the punch packet. That
 * is all.
 *
 * HOW A PUNCH WORKS, AND WHERE THIS FITS
 *
 * A home router drops an unsolicited packet from the internet, but it remembers
 * an outgoing one and lets the reply back in. So if both peers send to each
 * other at the same moment, each one's outgoing packet teaches its own router
 * to expect the other's, and the two packets cross in the middle. After that
 * the path is open in both directions and the ordinary netchan handshake runs
 * over it.
 *
 * The recipe the application follows:
 *
 *   1. Learn your public address with nc_stun_* and register it with a
 *      rendezvous, which tells each peer the other's address.
 *   2. nc_reach_session() derives the shared token and which peer becomes the
 *      netchan client, from the pair of addresses, the same on both sides.
 *   3. Fire a punch at the peer every ~200 ms, for a few seconds, on your own
 *      clock. The first one opens your router; it takes both sides firing for
 *      the path to complete.
 *   4. When a punch arrives, reply with a punch-ack and remember the address it
 *      came from: that is where the peer is really reachable, which a NAT may
 *      map to a different port than STUN predicted. When an ack arrives, the
 *      round trip is proven and the path is open.
 *   5. The client calls netchan_connect to that address; the other side opens a
 *      server and accepts. netchan's own path challenge then validates the
 *      address before the session commits to it, so the punch has to open a
 *      path, not be trusted.
 *
 * WHAT THIS IS NOT
 *
 * It is not a rendezvous server. Introducing two peers, so each learns the
 * other's address, is the application's to arrange, over whatever channel it
 * already trusts; the CONNECT_REDIRECT frame carries an address and a
 * connection id from one peer to another for exactly this. This file starts
 * once both addresses are known.
 *
 * It does not beat a symmetric NAT. A router that maps the same inside address
 * to a different outside port for every destination hands STUN a port that is
 * not the one it will use toward the peer, so the punch is fired at the wrong
 * door. Cone NATs, which most home routers are, keep one mapping and are
 * punched through here; the symmetric case needs port prediction or a relay,
 * which is a larger thing and not attempted.
 *
 * THE TOKEN IS NOT A SECRET
 *
 * Both peers derive the same token from their two public addresses, so it is
 * known to anyone who knows both addresses, the rendezvous included. It only
 * tells one punching session from another on a shared port; it proves nothing.
 * What proves a peer is really at an address is that the punch reaches it and
 * the ack returns, the same return-routability the core's path challenge rests
 * on, and the netchan handshake that follows is what the session actually
 * trusts. So a stray or forged punch wastes a packet and no more.
 */

#ifndef NC_REACH_H
#define NC_REACH_H

#include <stdint.h>
#include <stddef.h>

#include "nc_addr.h"

#define NC_REACH_OK   (0)
#define NC_REACH_ERR  (-1)

/* The first four bytes on the wire read "NCR1" in a hex dump, so a punch that
 * lands on a game port is told from a datagram at a glance and dropped, rather
 * than half-parsed as one. */
#define NC_REACH_MAGIC      0x3152434eu

#define NC_REACH_TOKEN_LEN  8
#define NC_REACH_PACKET_LEN 16          /* magic, kind, pad, then the token */

/** What a punch packet is. */
enum {
    NC_REACH_PUNCH = 1,       /* I am firing at your address to open my NAT */
    NC_REACH_PUNCH_ACK = 2,   /* your punch reached me, so my side is open */
};

/** Which netchan role this peer takes once the path is open. Derived from the
 * address pair so exactly one of the two becomes the client. */
enum {
    NC_REACH_ROLE_CLIENT = 0, /* call netchan_connect over the opened path */
    NC_REACH_ROLE_SERVER = 1, /* netchan_open(1), then netchan_accept */
};

/*
 * A punching session between my public address and the peer's. Built once, held
 * by value: there is no socket in it and no clock, so nothing to tear down. The
 * fields are an implementation detail; treat it as opaque and start it with
 * nc_reach_session rather than filling it by hand.
 */
struct nc_reach {
    struct nc_addr peer;                  /* the address to fire punches at */
    uint8_t token[NC_REACH_TOKEN_LEN];    /* the shared session token */
    int role;                             /* NC_REACH_ROLE_* for this peer */
};

/**
 * Derive the session from my public address and the peer's, both learned from
 * STUN and exchanged through a rendezvous. Fills *s with the shared token, the
 * role this peer takes, and the peer address to punch toward. Returns
 * NC_REACH_OK, or NC_REACH_ERR if either address is unset or the two are
 * identical, which cannot form a session because both peers would pick the same
 * role. The derivation does not depend on argument order: both peers compute
 * the same token and agree on which one is the client.
 */
int nc_reach_session(struct nc_reach *s, const struct nc_addr *me,
                     const struct nc_addr *peer);

/**
 * Build a punch, or a punch-ack, for this session. kind is NC_REACH_PUNCH or
 * NC_REACH_PUNCH_ACK. Returns NC_REACH_PACKET_LEN, or NC_REACH_ERR if the
 * buffer is too small or kind is neither. The caller sends it to s->peer and
 * owns the cadence, firing one every couple hundred milliseconds for a few
 * seconds, exactly as a beacon owns its announce timer.
 */
long nc_reach_build(void *out, size_t outlen, const struct nc_reach *s, int kind);

/**
 * Classify an inbound datagram against this session. Returns NC_REACH_PUNCH or
 * NC_REACH_PUNCH_ACK for a packet that carries our magic and our token, or
 * NC_REACH_ERR for anything else, including a punch for a different session, so
 * a datagram on a shared port is rejected rather than half-read.
 *
 * The source address is deliberately not an argument. A valid punch tells you
 * where the peer is really reachable by the address it arrives from, which a
 * NAT may map to a port STUN did not predict, so the caller reads that from the
 * socket and both punches back and later connects to it. The token is not a
 * secret and this check is not trust: it demultiplexes sessions, and the
 * netchan handshake over the opened path is what the session trusts.
 */
int nc_reach_parse(const struct nc_reach *s, const void *pkt, size_t len);

#endif /* NC_REACH_H */
