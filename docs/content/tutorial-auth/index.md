---
title: Tutorial: authentication
weight: 6
abstract: Adding the encrypted transport and an ssh-shaped login to a working session.
category: manual
---

Two independent layers sit above the core. Encryption comes first, then a login carried as ordinary messages.

`nc_crypto` is a transport decorator. Before netchan speaks, the two ends run an X25519 handshake; once `nc_crypto_ready()` reports the keys are set, every datagram is sealed with XChaCha20-Poly1305 on the way out and opened on the way in. The crypto handshake always finishes before netchan's own, so even the `CONNECT_INIT` travels sealed. The core is never aware of any of it.

    struct nc_crypto cr;
    nc_crypto_init(&cr, /*role=*/0, &(struct nc_crypto_cfg){0});

    /* until nc_crypto_ready(), send nc_crypto_handshake_packet() output */
    long s = nc_crypto_seal(&cr, pkt, len, sealed, sizeof sealed);
    long o = nc_crypto_open(&cr, rx, rxlen, plain, sizeof plain);
    if (o > 0) netchan_feed(c, plain, o, &from);

Give the responder a long-term identity key and the handshake authenticates it as well: a second Diffie-Hellman folds the identity key into the derivation, so the first packet that opens is proof the peer holds the key, with no signature on the wire and no extra round trip. Deciding whether to trust a presented key is deliberately not this layer's job; a `verify_peer` callback answers that, and comparing a key against one recorded earlier is trust on first use, the same model ssh uses for host keys.

`nc_auth` is the login on top, a pair of state machines with no socket and no netchan in them. Beside publickey and password it carries a third method, an interactive one that asks the player questions the server wrote. That is how a client with no account registers for one, and how an emailed code or a one-time token gets answered. See [registration](../registration/). Messages go in through `nc_auth_feed()` and come out for the caller to carry on a reliable channel. `keystore` reads and writes the on-disk formats, `known_hosts`, `host_key`, `authorized_keys`, `passwd`, and the client key file, all plain text an operator can read and edit. The client's signature covers the `nc_crypto` session id, so a signature captured by one server cannot be replayed at another. `examples/auth/` wires the whole stack together.

