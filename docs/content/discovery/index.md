---
title: Finding servers
weight: 13
abstract: A beacon packet for the local network, and why anything a meta-server should trust goes over HTTPS instead.
category: design
draft: false
---

Two problems arrive together because they have the same shape. A player on a
home network wants the list of games running in the house, and a public server
wants a listing site to know it exists. Both are answered by a small packet sent
into the dark, with no session behind it and no reply expected.

`nc_beacon` implements this. `discovery/nc_beacon.c` builds and parses the
packet, and `tests/test_nc_beacon.c` feeds it the cases that matter: a browser
from last year reading a server from this year, and a packet from whoever
happens to be on the link.

## Not on netchan

A beacon has no handshake, no channels, no ordering, no reliability, and no
peer. It shares nothing with the protocol core except a socket, so it is a
sibling of netchan rather than a feature of it: `nc_beacon` builds one packet
and parses one packet, and knows nothing else.

It carries its own magic and lives on its own port, so a beacon that lands on a
game port is rejected as garbage rather than half-parsed as a datagram. The core
never sees one.

The body is [IDL](../encoding/)-encoded, and forward compatibility is the whole
reason. A server browser shipped last year has to read a beacon from a server
shipped this year, show the fields it understands, and quietly ignore the rest.
A reader that skips a tag it has never heard of gives that for nothing, and the
alternative is a browser that goes blind every time a server adds a field.

The whole packet stays well inside 512 bytes, so it survives any link without
fragmenting.

## What a beacon says

| Field | Carries |
|---|---|
| `game` | which game this is, so foreign traffic is discarded at once |
| `wire` | the netchan protocol version |
| `version` | the game version, major and minor only |
| `name` | what the server calls itself, UTF-8 |
| `port` | where to actually connect |
| `players`, `capacity` | how full it is |
| `modes` | game modes, as bits the application defines |
| `flags` | credentials required, registration open, and similar |
| `contact` | free text, optional: who runs it |
| `key` | the server's identity public key, optional |
| `instance` | a random number chosen at startup |

`players` and `capacity` earn their place by being the field a person actually
reads. A list that cannot say "3/16" is a list nobody sorts.

`port` is separate because the beacon rarely comes from the port players connect
to, and on a machine running two shards it certainly does not.

The flags for credentials and registration tie the browser to
[registration](../registration/). A client can grey out a server it has no
account on, or offer to create one before connecting rather than after the
conversation has already started.

`instance` is a random number the server picks when it starts. It lets a browser
tell two servers apart when their names collide, notice that a server restarted
rather than that a new one appeared, and collapse the duplicate beacons a
machine with two network interfaces sends of itself.

`key` authenticates nothing on its own. What it does is let a client match a
beacon against a `known_hosts` entry it already holds, which is the trust path
the registration design assumes: the key arrives from the installer or over
HTTPS, and the beacon only says which of the keys you already trust is on this
address.

### What it deliberately does not say

The patch version. Major and minor answer the only question a client has, which
is whether it can play here. A patch level tells anyone listening precisely
which published bugs this build has, and buys nothing in return. The netchan
wire version is separate from the game version because they answer different
questions and move on different schedules.

The administrator's account name. It is tempting, because "Bob's server" is
genuinely useful in a house with three of them. But a real login name broadcast
every few seconds hands an attacker half a credential, and it is the half that
never rotates. `contact` is free text with no relationship to any account: the
same sentence to a human, nothing to a scanner.

## Announcing and asking

Servers beacon on a slow timer, somewhere between five and fifteen seconds, so a
browser left open stays current without anyone paying much for it.

Clients also send a probe when they start, and servers answer it directly, so a
browser that just opened shows something immediately instead of waiting out a
full interval. The two halves cost little together and each covers the other's
weakness: the probe is fast, and the timer catches a server that was not
listening when the probe went past.

The probe has a sharp edge. A small packet drawing a larger reply to whatever
address it claims is a reflector, and pointing it at a stranger is the oldest
trick there is. What keeps this a local feature is that a server answers a probe
only from its own link or subnet, and rate-limits the answers it does send.

That is the same posture the rest of the design takes: the concern is abuse and
griefing rather than a funded attacker, and the tools are cheap limits plus an
operator who can read a log. A server that logs refused probes can point the
same deny daemon at them that it already points at refused registrations.

## The meta-server

A public server wants a listing site to know it is alive. The obvious move is to
send it the same beacon over the internet, and the obvious follow-up is to sign
that beacon so the site knows who sent it. The follow-up is worth less than it
looks.

A signature proves a packet came from a key. It does not prove the sender holds
the address written inside it, so a captured announce replays from anywhere. It
does not prove the server is reachable, or that it is still running, or that the
port it named answers. A meta-server has to connect back to establish any of
that, and once it connects back, the connection has done everything the
signature was supposed to do.

It is not free either. A netchan server holds an X25519 identity secret and
nothing else, so signing means giving it an Ed25519 key it does not currently
have, somewhere to keep it, and a story for rotating it.

So the recommendation is plain. Send updates to a meta-server over HTTPS, with
whatever credential the operator was going to need anyway. TLS gives
authenticity and confidentiality against exactly the attacker who matters here,
the one between the server and the listing site, and it is the arrangement every
published game already uses. Publish server identity keys the same way, which is
also how a client's `known_hosts` gets refreshed.

**Do not use the beacon where server authenticity matters.** It is a local
convenience with no authentication in it, and adding a signature would not make
it one.

If a UDP path to a meta-server is wanted anyway, the shape is a timestamp, the
identity key, and a signature over both, which bounds a replay to the clock
skew the site is willing to accept. The meta-server still has to connect back.
That is worth writing down and not worth building until something needs it.

## What netchan does not do

`nc_beacon` builds a packet and parses a packet. It does not open a socket, set
a broadcast or multicast option, choose an address, or run a timer, for the same
reason the core never names a socket: the application owns its event loop and
its address family, and a discovery module that grabbed either would be the one
piece of this library that could not be dropped into an existing game.

It does not speak HTTP, hold a meta-server credential, or decide what a game
mode bit means. It does not maintain the list a browser shows, because
collecting beacons, ageing them out, and sorting them is a user interface
question wearing a networking hat.
