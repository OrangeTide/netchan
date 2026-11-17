---
title: Punching a path between two NATs
weight: 16
abstract: Opening a path between two peers who both sit behind a router that will not forward a port, by firing packets at once so the two NAT bindings open together.
category: design
draft: false
---

Two people on home networks want to play together, and the [nat helpers](../nat/)
have done their part: each has learned its own public address with STUN, and a
rendezvous has told each the other's. If one of them had forwarded a port, that
would be enough; a connection to the forwarded address would arrive. Neither
has. Both sit behind a router that drops an unsolicited packet from the
internet. This page is the design for the piece that opens a path anyway.

`nc_reach` is the hole-punch half of reachability. Like the beacon and the nat
helpers, it is a sibling of the protocol core rather than a feature of it: it
opens no socket, runs no timer, and picks no address. It derives a session from
the two addresses, and it builds and parses one small packet. The application
owns the clock that fires the punches and the socket they go out on, and hands
the opened path to `netchan_connect`.

## Why firing at once opens a path

A home router drops a packet that arrives from the internet unbidden, but it
remembers a packet that left and lets the reply back in. That asymmetry is the
whole of the trick. If both peers send to each other at the same moment, each
one's outgoing packet teaches its own router to expect a reply from the other
address, and the two packets cross in the middle. The first one each router
sees is still outbound, so each opens its own door; by the time the other
peer's packet arrives, the door is open and it is let through.

After that the path is open in both directions, and nothing more about it is
special. The ordinary `CONNECT_INIT` and `CONNECT_ACCEPT` handshake runs over
it exactly as it does on a LAN, and the [core's path challenge](../reachability/)
validates the address before the session commits to it. The punch opens a path;
it does not ask to be trusted.

## The shape of the layer

The whole of `nc_reach` is three calls and one packet, so the application keeps
the parts that need a socket and a clock, and the library keeps the parts that
do not.

- `nc_reach_session` takes this peer's public address and the other's and
  derives the shared token, the address to punch toward, and which of the two
  becomes the netchan client. It depends on nothing but `nc_addr`.
- `nc_reach_build` writes a sixteen-byte punch, or a punch-ack, for that
  session. The caller sends it and owns the cadence.
- `nc_reach_parse` reads an inbound datagram and says whether it is a punch, an
  ack, or not ours at all.

The packet carries a magic that reads `NCR1` in a hex dump, a kind byte, and the
eight-byte token. It is small on purpose: a punch is a packet sent to open a
door, not to carry anything.

## The recipe

The application drives it, because the application owns the socket and the
clock. The steps are the same on both peers, with one asymmetry that the
session resolves.

1. Learn your public address with `nc_stun_*` and register it with a rendezvous,
   which tells each peer the other's address.
2. Call `nc_reach_session` with the pair. Both peers derive the same token and
   agree on which one is the client, because the derivation does not depend on
   the order of the two addresses.
3. Fire a punch at the peer every couple hundred milliseconds, for a few
   seconds, on your own clock. The first one opens your router; it takes both
   sides firing for the path to complete.
4. When a punch arrives, reply with an ack and remember the address it came
   from. That source address is where the peer is really reachable, which may be
   a different port than STUN predicted, so it is the address to use from here
   on. When an ack arrives, the round trip is proven and the path is open.
5. The client calls `netchan_connect` to that address; the other peer opens a
   server and accepts. The handshake, and the core's path challenge under it,
   carry on from there.

The role in step two keeps the two peers from both trying to be the client. It
is derived from the address pair, so exactly one of them connects and the other
listens, with no extra message to agree on it.

## What stays out, and why

The rendezvous is not here. Introducing two peers, so each learns the other's
address, is the application's to arrange over whatever channel it already
trusts. The `CONNECT_REDIRECT` frame already carries an address and a connection
id from one peer to another, which is the shape a rendezvous answer takes. This
layer starts once both addresses are known, so it owes nothing to how they were
learned.

The clock and the socket are not here either, for the reason the core never
names a socket and the beacon never runs its own announce timer. A module that
grabbed the event loop would be the one piece of netchan that could not drop
into a game that already has one. The cadence of the punches, the few seconds
before giving up, and the datagrams themselves are the application's, and this
file only says what to send and what an inbound packet means.

## Two honest limits

A symmetric NAT is not punched through. A router that maps the same inside
address to a different outside port for every destination hands STUN a port it
will not reuse toward the peer, so the punch is aimed at a door that is not
there. Most home routers keep one mapping and are punched through here; the
symmetric case needs port prediction or a relay server, which is a larger thing
and not attempted. A connection that cannot be punched falls back to a relay the
application provides, or to one peer forwarding a port by hand.

The token is not a secret. Both peers derive it from their two public addresses,
so anyone who knows both addresses, the rendezvous included, can derive it too.
It only tells one punching session from another on a shared port. What proves a
peer is really at an address is that the punch reaches it and the ack returns,
and the netchan handshake that follows is what the session actually trusts. A
stray or forged punch costs a packet and nothing more.
