---
title: Architecture
weight: 2
abstract: The seam that lets one object file run over UDP, a WebSocket, or an encrypted tunnel.
category: manual
---

The protocol core never names a socket. A received datagram enters through `netchan_feed()` and an outgoing one leaves through `netchan_send_next()`, each tagged with an opaque `struct nc_addr` that the core copies and compares but never interprets. Everything below that line, the socket, the address family, the encryption, is the caller's to provide.

That single seam is what lets the same core serve every transport. The layers stack, and each builds without the ones above it.

      application
          |
       netchan        reliable ordered delivery, channels, flow control   (src/)
          |
      nc_crypto       X25519 + XChaCha20-Poly1305, optional                (crypto/)
          |
       nc_udp         the only file that has ever heard of sockaddr        (transport/)
          |
      the network

Encryption is a decorator. `nc_crypto` seals a datagram on its way out and opens it on the way in, and the core never learns it happened. Authentication is a conversation carried on an ordinary reliable channel, so the core does not know that either. netchan has no login primitive; a login is messages the application sends like any other.

A connection moves through a small state machine. It opens with a handshake, reaches `CONNECTED` where channels carry data, and ends either by an explicit disconnect or by the idle timeout.

![states](states.svg)

