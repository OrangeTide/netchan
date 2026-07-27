---
title: Tutorial: extending the protocol
weight: 7
abstract: Adding your own frame types without breaking a peer that predates them.
category: manual
---

Because the core names no socket, a new transport is just a pair of functions that move bytes. Seal or frame an outgoing datagram from `netchan_send_next()` however the medium requires, and hand what arrives to `netchan_feed()`. The `nc_ws` transport does exactly this for browsers: it speaks the RFC 6455 WebSocket framing so a browser client reaches an unmodified UDP server. `ws_gateway` relays between the two, presenting each browser as an ordinary netchan peer.

The address a datagram carries is opaque to the core, so a transport is free to define `struct nc_addr` however it likes, as long as it packs the bytes on the way in and unpacks them on the way out. A transport that has no meaningful source address, such as a single fixed tunnel, passes `NULL` and the core copes.

To carry structured messages rather than raw bytes, use the encoding layer below. It needs no change to the core either.

