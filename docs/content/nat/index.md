---
title: Reaching a server behind a NAT
weight: 14
abstract: Learning your public address with STUN, writing it short as a room code, and why opening a router's port is out of scope.
category: design
draft: false
---

Two people on home networks want to play together over the internet. Each sits
behind a router that hands out a private address and rewrites it on the way out,
so neither knows the address a friend would have to reach, and a code typed over
voice chat is easier to get wrong than right. `nc_nat` answers the first problem
and the second, and deliberately not the third.

`nat/nc_nat.c` is two small helpers. `nc_stun_*` builds a STUN binding request
and parses the reply, so a host learns its own public address. `nc_roomcode_*`
turns that address and port into a short string with a check against typos, and
back again. `tests/test_nc_nat.c` feeds each the cases that matter: a STUN reply
that is not ours or is forged, and a room code read the forgiving way a person
types it or mistyped past its check.

## Not on netchan

Like [the beacon](../discovery/), these are siblings of the protocol core rather
than features of it. Neither opens a socket or runs a timer, for the same reason
the core never names one: the application owns its event loop and its address
family. STUN builds one packet and parses one; the caller sends it, retries it,
and times it out. The room code is a pure function with no IO in it at all.

Both speak a raw IPv4 address and port rather than `struct nc_addr`, so the file
depends on nothing else in netchan and can be vendored on its own. The
application bridges to `nc_addr` with the `nc_udp` helpers when it is ready to
connect.

## Learning your address with STUN

A STUN server answers one question: what address did my packet come from? The
answer is the public address and port the router mapped for you, which is what a
friend has to reach. `nc_stun_build_request` builds the 20-byte binding request
of RFC 5389, and `nc_stun_parse_response` reads the address back out of the
reply.

This is useful when a port is already forwarded, whether by hand or by a router
friendly enough that the outgoing STUN packet leaves the door ajar for the reply
and the game traffic behind it. It does not open a port, and it is not NAT
traversal; the last section says what that distinction means and what the real
thing would take.

### The transaction id has to be random

The reply is matched to the request by a 96-bit transaction id the caller
chooses. That match is the only thing standing between you and a forged "your
address is ..." answer from somebody off the path, so the id has to come from a
real random source, not a counter or a clock. netchan ships one in `nc_crypto`;
a caller that vendors `nc_nat` alone supplies its own. The parser drops any reply
whose id, magic cookie, or message type is not the one it expects, which the test
checks by flipping one byte of each.

Only IPv4 is reported. A host reachable over IPv6 does not need STUN to learn its
address in the first place, and the room code is IPv4-only regardless; a wider
code would change both together.

## Writing it short: the room code

A public address is awkward to pass to a friend. Forty-eight bits of address and
port become a short string instead:

| Bits | Carries |
|---|---|
| 32 | the IPv4 address |
| 16 | the port |
| 12 | a check against typos |

That is 60 bits, which is exactly twelve characters of Crockford's base 32,
written in three groups of four: `FACE-8008-5309`. Crockford's alphabet has no I,
L, O or U, so no two symbols look or sound alike, and the decoder folds the
mistakes a person still makes. Case is ignored, the dashes are optional, and the
letters O, I and L read as 0, 1 and 1. A caller tries a code before it tries a
plain `host:port`, so a mistyped code is reported as a bad code rather than fed
to the resolver and dialed somewhere real.

The twelve check bits are the point of the exercise. They are not a checksum for
corruption on the wire, where there is none yet, and not a MAC: they catch the
character a person misheard or fat-fingered before any packet is sent. The test
nudges each data character to every other symbol in the alphabet and confirms
that all of them fail the check, so a single typo never decodes to some other
valid address.

The codec carries the bits and nothing more. A zero address encodes and decodes
like any other, because whether an address is worth handing out is the caller's
question, not the codec's.

### A room code is an address, not a secret

Anyone a code is given to, or who reads it off a stream, learns where you are.
It is your address written short, so treat it like your address: a convenience
for reaching friends, not a credential, and not a thing to show an audience you
have not thought about. The check digits guard against typos, never against
someone who means to connect.

## What `nc_nat` does not do

**It does not open a router's port.** Asking a router to forward a port by
itself is out of scope for netchan. It is IO and timers all the way down, it is
the largest new attack surface a networking library could add because it reaches
out to a device on the LAN and parses what it says back, and it works on fewer
routers every year while doing nothing at all against carrier-grade NAT. The
cost is high, the benefit is shrinking, and it is not a goal here.

**It does not punch through a router that will not forward a port.** That is the
real answer to two peers who both sit behind uncooperative NATs, and it is a
larger thing than an address helper: both peers learn their public addresses,
which `nc_stun_*` already does, a rendezvous server tells each the other's, and
both start sending at once so each side's outgoing packet opens the path for the
other's. The groundwork is present. The `CONNECT_REDIRECT` frame and the
`NETCHAN_EV_REDIRECT` event already carry an address and a connection id from one
peer to another, and `nc_addr` stays opaque throughout, so the punched path can
be v4 or v6 without the mechanism caring. What is missing is the rendezvous
server, the both-sides-at-once timing, and the retry policy for the first packets
crossing in flight. That is written down in `TODO.md` as the direction these
helpers point at, so the choice to stop at address learning for now stays an
informed one.
