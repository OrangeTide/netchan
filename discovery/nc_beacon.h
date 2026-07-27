/* nc_beacon.h : the packet a server sends into the dark so it can be found */

#ifndef NC_BEACON_H
#define NC_BEACON_H

#include <stdint.h>
#include <stddef.h>

/*
 * A player on a home network wants the list of games running in the house, and
 * a public server wants a listing site to know it exists. Both are answered by
 * a small packet with no session behind it and no reply expected, which is why
 * they are one thing and why that thing is not netchan.
 *
 * A beacon has no handshake, no channels, no ordering, and no peer. It shares
 * nothing with the protocol core except a socket, so this is a sibling of
 * netchan rather than a feature of it. The core never sees one: a beacon
 * carries its own magic and lives on its own port, and one that lands on a game
 * port is rejected as garbage rather than half-parsed as a datagram.
 *
 * THIS FILE BUILDS AND PARSES. THAT IS ALL.
 *
 * It does not open a socket, set SO_BROADCAST or join a multicast group, pick
 * an address, or run a timer, for the same reason the protocol core never names
 * a socket: the application owns its event loop and its address family, and a
 * discovery module that grabbed either would be the one piece of this library
 * that could not be dropped into a game that already exists.
 *
 * DO NOT TRUST A BEACON
 *
 * Nothing in it is authenticated and nothing can be. Anyone on the link can
 * send one saying anything, and a signature would not help: it would prove a
 * packet came from a key, not that the sender holds the address inside it, nor
 * that the server is reachable, nor that it is still running. Learning any of
 * that means connecting to the server, and the connection then does everything
 * the signature was for.
 *
 * So a beacon is a hint about where to look, never a statement of fact. Where
 * server authenticity matters, send updates to a meta-server over HTTPS and
 * publish identity keys the same way. See the design page for the longer
 * version of this argument.
 *
 * THE PROBE IS A REFLECTOR AND THE CALLER OWNS THE FIX
 *
 * A probe is small and draws a larger reply to whatever address it came from,
 * which is the oldest trick there is for pointing traffic at a stranger. What
 * keeps this a local feature is that a server answers a probe only from its own
 * link or subnet and rate-limits the answers it sends. Neither check can live
 * here, because neither is possible without knowing what an address is. Anyone
 * answering probes has to make them.
 */

/* The first four bytes on the wire read "NCB1" in a hex dump. */
#define NC_BEACON_MAGIC   0x3142434eu

#define NC_BEACON_MAX          512      /* fits any link without fragmenting */
#define NC_BEACON_MAX_NAME      64
#define NC_BEACON_MAX_CONTACT   64

#define NC_BEACON_OK  (0)
#define NC_BEACON_ERR (-1)

/** What a packet is. */
enum {
    NC_BEACON_ANNOUNCE = 1,   /* a server saying it is here */
    NC_BEACON_PROBE = 2,      /* a client asking who is */
};

/* Flags a browser can act on without understanding the game. */
#define NC_BEACON_F_CREDENTIALS  0x01   /* a login is required to play */
#define NC_BEACON_F_REGISTER     0x02   /* and an account can be made here */

/**
 * One server, as advertised.
 *
 * The strings are held rather than pointed at, so a parsed beacon is a value a
 * browser can keep in a list without owning the packet it came from.
 *
 * A zero field is left out of the packet entirely, so a server that fills in
 * three of these sends a very small beacon, and a reader that has never heard
 * of a field skips it. That is the whole reason the body is tagged: a browser
 * shipped last year has to read a beacon from a server shipped this year, show
 * what it understands, and quietly ignore the rest.
 */
struct nc_beacon {
    uint32_t game;        /* which game, so foreign traffic goes at once */
    uint16_t wire;        /* the netchan protocol version */
    uint16_t major;       /* the game version. No patch level: see below */
    uint16_t minor;
    uint16_t port;        /* where to actually connect */
    uint16_t players;
    uint16_t capacity;
    uint32_t modes;       /* game modes, as bits the application defines */
    uint32_t flags;       /* NC_BEACON_F_* */
    uint32_t instance;    /* a random number chosen at startup */
    char     name[NC_BEACON_MAX_NAME + 1];
    char     contact[NC_BEACON_MAX_CONTACT + 1];
    uint8_t  key[32];     /* the server's identity public key */
    int      has_key;
};

/*
 * TWO THINGS A BEACON DELIBERATELY DOES NOT CARRY
 *
 * The patch version. Major and minor answer the only question a client has,
 * which is whether it can play here. A patch level tells everyone listening
 * precisely which published bugs this build has and buys nothing back. The
 * netchan wire version is separate because it answers a different question and
 * moves on a different schedule.
 *
 * The administrator's account name. "Bob's server" is genuinely useful in a
 * house with three of them, but a real login name broadcast every few seconds
 * hands an attacker half a credential, and it is the half that never rotates.
 * contact is free text with no relationship to any account: the same sentence
 * to a human, nothing to a scanner.
 *
 * ABOUT THE IDENTITY KEY
 *
 * It authenticates nothing on its own, and putting it here does not make the
 * beacon trustworthy. What it does is let a client match a beacon against a
 * known_hosts entry it already holds. The trust arrives from the installer or
 * over HTTPS; the beacon only says which of the keys you already trust is on
 * this address today.
 */

/**
 * Build an announcement. Returns its length, or NC_BEACON_ERR if it does not
 * fit in outlen or in NC_BEACON_MAX.
 */
long nc_beacon_build(void *out, size_t outlen, const struct nc_beacon *b);

/**
 * Build a probe: which game is being looked for, and nothing else. A client
 * sends one when it opens so a browser shows something at once instead of
 * waiting out a server's announce interval.
 */
long nc_beacon_probe(void *out, size_t outlen, uint32_t game);

/**
 * What kind of packet this is, without decoding it. Returns NC_BEACON_ANNOUNCE,
 * NC_BEACON_PROBE, or NC_BEACON_ERR for anything that is not a beacon at all.
 */
int nc_beacon_kind(const void *pkt, size_t len);

/**
 * Decode an announcement into *b, which is overwritten whether this succeeds or
 * not. Returns NC_BEACON_ERR on anything malformed, on a string longer than it
 * has room for, and on a probe.
 *
 * Rejecting rather than truncating is deliberate. A form comes from a server
 * the player chose to talk to, and an operator who misconfigures one can test
 * it. A beacon comes from whoever is on the link.
 */
int nc_beacon_parse(struct nc_beacon *b, const void *pkt, size_t len);

/** The game id from a probe, so a server can tell whether it is being asked
 * for. Returns NC_BEACON_ERR if this is not a probe. */
int nc_beacon_probe_game(const void *pkt, size_t len, uint32_t *game);

#endif /* NC_BEACON_H */
