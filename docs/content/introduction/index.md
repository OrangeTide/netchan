---
title: Introduction
weight: 1
abstract: What netchan is, what it carries, and the limits it accepts on purpose.
category: manual
---

netchan carries several independent streams of messages over one unreliable datagram link. A channel is reliable and ordered, unreliable and unordered, or a reliable byte stream, and many channels share one connection and one socket. A game can put player input on an unreliable channel where a dropped packet is better than a late one, and world state on a reliable channel beside it, without opening a second socket or running a second protocol.

The library is portable C11 with no dependencies beyond the C library. It allocates one connection object at open and nothing after that. It never calls a socket function, so the same object file runs over UDP on a desktop, over a WebSocket in a browser, and over an encrypted tunnel in between. It is built to be vendored: you copy the directories you want into your own tree and build them with your own build system.

Some limits are deliberate. There is no congestion control, so a sender that outruns the link will lose packets rather than back off. There is no built-in defense against a forged source address or a traffic amplifier, which is the transport's and the application's concern. A connection id identifies a session but authenticates nothing; the `crypto/` and `auth/` layers exist for when identity matters.

