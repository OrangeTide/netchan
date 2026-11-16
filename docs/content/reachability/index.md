---
title: Reachability
weight: 15
abstract: Folding address learning into the connection, so STUN, room codes, redirect, and migration all feed one validated address path instead of bolting on beside it.
category: design
draft: false
---

Two people on home networks want to play together, and neither knows the address
a friend has to reach. The [nat helpers](../nat/) answer that today as a sibling
of the core: STUN learns a public address and a room code writes it short. They
work, but they stand beside netchan rather than inside it, speaking their own
currency of raw IPv4 and port while everything else in netchan trades in
`nc_addr`.

This page is the design for closing that gap. The goal is not a bigger pile of
helpers. It is to make reaching a peer a part of what a netchan connection does,
with one address type, one place that validates an address, and one lifecycle
that STUN, room codes, redirect, and migration all flow through.

## What the core already knows about addresses

Most of the machinery is already here, and it already speaks `nc_addr`.

- A connection is opened to an `nc_addr` (`netchan_connect`), and it keeps the
  peer's address as an `nc_addr` (`peer_addr`).
- The redirect frame is `nc_addr`-native and implemented. `CONNECT_REDIRECT`
  carries an address whose bytes are exactly the `nc_udp` packing, so the core
  reads it with a straight copy and raises `NETCHAN_EV_REDIRECT`. One peer can
  already tell another "connect over there instead".
- The core already migrates. When a datagram arrives from an address that
  differs from `peer_addr`, the core adopts the new one, so a connection follows
  a peer whose address changes under it.

So netchan already has an address as the living currency of a connection, a way
to hand one peer another's address, and address rebinding. What it does not have
is a safe way to trust a new address, and one outlier that refuses the common
currency.

## One address currency

The outlier is the nat layer. It reports a learned address as a 32-bit integer
and a port, and its room code encodes the same pair, because it was written to
depend on nothing else in netchan and be vendored alone. That independence is
what makes it read as an extension.

The design gives it the common currency instead.

- STUN fills an `nc_addr`. A binding reply names the public address the router
  mapped, and that address is already in the `nc_udp` packing, so it drops into
  `netchan_connect`, into a `CONNECT_REDIRECT` frame, or into `peer_addr` with no
  translation at all.
- The room code encodes and decodes an `nc_addr`. For an IPv4 address it is the
  same twelve-character code as today. Because the type is now `nc_addr` rather
  than a bare IPv4 integer, a wider code for IPv6 later is a change to the code
  format, not a break in the function's shape. The "IPv4 forever" wall that the
  nat page records comes down.

There is one packing of an IPv4 address into an `nc_addr`, and today three places
know it: `nc_udp`, the redirect handler that copies it straight, and the nat
layer that hand-rolls its own bytes. The design collapses those into a single
pack-and-unpack helper that all of them call, so the layout is defined once. With
that, the nat layer stops being a parallel library and becomes the addressing
companion to `nc_udp`, the part of the transport layer that learns and names an
address rather than sends to one.

      application
          |
       netchan          reliable ordered delivery, channels   (src/)
          |
      nc_crypto          encryption, optional                  (crypto/)
          |
       nc_udp  +  nat    send to an address; learn and name one (transport/)
          |
      the network

## A validated address path

Here is the piece that genuinely belongs in the core, and the reason reachability
cannot simply be a well-behaved sibling.

The core today trusts an address on sight. It rebinds `peer_addr` to any source
address a datagram claims, and it retargets to any address a redirect frame
names. Neither is checked. An attacker who learns a connection id can redirect
the flow to a victim, and a forged source address can move a session to a machine
that never asked for it. Address rebinding without a check is a hijack and
amplification primitive, not a feature.

Reaching a peer across the internet makes new addresses routine: a STUN result,
a room code a person typed, a redirect from a rendezvous, a migration as a NAT
rebinds. Every one of them is an address the core is being asked to act on, and
every one needs the same guarantee, that the peer is really at the address
before the connection commits to it.

So the design adds one mechanism: a return-routability challenge. Before the core
trusts a new or claimed address, it sends a small nonce there and does not act
until the nonce comes back. A genuine peer echoes it; a spoofed source never sees
it. The same challenge covers migration, redirect, and the moment a punched path
first carries a packet, and it is the same token that validates a client's
address before a server commits connection state to it. One check, used
everywhere an address changes hands.

This belongs in the core because the address it protects, `peer_addr`, and the
redirect handler it guards both live there. It costs the core nothing it was
built to avoid: the nonce is a few bytes over the frame path that already exists,
with no socket and no knowledge of what an address is, so the transport-agnostic
core stays transport-agnostic and the challenge works the same over UDP, a
WebSocket, or a punched path.

## Reachability as a phase of connecting

With one currency and one validated path, reaching a peer becomes a named stage
of establishing a connection rather than something arranged off to the side.

1. A host learns its own public `nc_addr` with STUN, and writes it as a room code
   or registers it with a rendezvous.
2. A client is introduced to the host's `nc_addr`, by typing the code or through
   a rendezvous that answers with a `CONNECT_REDIRECT`.
3. If both sit behind routers that will not forward a port, each sends to the
   other at once so the outgoing packet opens the return path, and the core's
   challenge confirms the path the moment it carries a packet.
4. The ordinary `CONNECT_INIT` and `CONNECT_ACCEPT` handshake runs over the open,
   validated path, exactly as it does on a LAN.

The core's state machine does not grow a NAT phase. A reachability driver does
the gathering, the introduction, and the simultaneous open, and hands the core a
validated `nc_addr`, which is what an application does by hand today. The
difference is that the driver is part of netchan, speaking its `nc_addr`, drawing
its randomness from the same source the core uses for connection ids, timing its
retries on the caller's clock, and surfacing its progress through the event
queue. It is woven in, not bolted on.

## What stays out of the core

Sockets and the STUN exchange stay out of `src/`. STUN owns a socket and, today,
speaks only IPv4, and a core that imported either would lose the properties that
are the point of it: the build that runs in a browser with no BSD sockets, the
one object file that serves every transport, and the dependency on nothing but
the C library.

Cohesion does not come from moving the socket upward into the core. It comes from
the shared address type, the one place that validates an address, and the
lifecycle that names where reaching a peer fits. The rendezvous and hole-punch
driver keeps its own sockets and timers, as the [discovery](../discovery/) beacon
keeps its own broadcast, and like the beacon it is a sibling that the core never
has to know about. What changed is that it now speaks the core's language and
feeds the core's validated path, so the seam between them is the same `nc_addr`
seam that already defines netchan, not a second vocabulary laid alongside.

## What ships today, and what this builds

Today the nat layer ships as the sibling the [nat page](../nat/) describes: STUN
and a room code over raw IPv4 and port, depending on nothing else and opening no
socket. This design keeps that layer's reach but changes its shape, and it lands
in stages so each is useful on its own:

- The nat layer speaks `nc_addr`, and one pack-and-unpack helper defines the IPv4
  packing for `nc_udp`, the redirect frame, STUN, and the room code together. No
  wire change.
- The core gains the return-routability challenge, so migration and redirect stop
  trusting an address on sight, and a server can demand an address proof before
  it commits connection state. This changes the wire, so it lands on a minor
  version.
- A reachability driver orchestrates rendezvous and the simultaneous open over
  the validated path, built on the two steps above.
