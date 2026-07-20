# netchan

Multiplexed reliable and unreliable channels over an unreliable datagram
transport, in portable C11, with optional encryption and an ssh-shaped login.
No dependencies, no package manager, and no build system of its own beyond a
single vendored `GNUmakefile`.

It is meant to be vendored. Copy the directories you want into your tree and
you have a protocol core, a UDP backend, transport encryption, and
authentication, each of which you can take or leave. See
[VENDORING.md](VENDORING.md).

## The Idea

The protocol core never names a socket. Datagrams arrive through
`netchan_feed()` and leave through `netchan_send_next()`, each tagged with an
opaque `struct nc_addr` that the core copies and compares but never
interprets. Everything below that line is somebody else's problem.

That one seam is what lets the same object file run over UDP on a desktop, a
WebSocket in a browser, and an encrypted tunnel in between, without the core
knowing which. Encryption is a decorator that wraps datagrams on their way
past, and the core is not aware it happened. Authentication is a conversation
held on an ordinary reliable channel, and the core is not aware of that
either.

```
  application
      |
   netchan          reliable ordered delivery, channels, flow control
      |
  nc_crypto         X25519 + XChaCha20-Poly1305, optional
      |
   nc_udp           the only file that has ever heard of sockaddr
      |
  the network
```

## Layout

| Directory | What it is | Needs |
|---|---|---|
| `src/` | the protocol core: `netchan.c/h`, `nc_addr.h` | nothing but libc |
| `transport/` | `nc_udp` (sockets) and `nc_ws` (RFC 6455 codec) | `nc_addr.h` |
| `crypto/` | `nc_crypto`, the encrypted transport decorator | monocypher |
| `auth/` | `nc_auth` (login state machines), `keystore` (on-disk formats) | monocypher |
| `third_party/` | vendored monocypher | — |
| `tests/` | the test suite | — |
| `examples/` | runnable programs, plus a vendored event loop they use | — |

Each layer builds without the ones above it. `src/` alone is a complete and
useful library.

## Build

```sh
make                      # library, tests, and examples
make run-tests            # the whole suite
make NETCHAN_EXAMPLES=0   # library and tests only
```

Binaries land in `_out/<triplet>/bin/`. A minimal `cc`-only build of the core
is kept in `Makefile.simple`, for anyone who wants to check that the
no-dependencies claim is real.

The suite is clean under AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
make clean
make CFLAGS="-fsanitize=address,undefined -g -O1" \
     LDFLAGS="-fsanitize=address,undefined"
make run-tests
```

## Using It

The application owns the socket. netchan says what to send and when to call
it again. How those bytes reach the wire is up to you.

```c
struct netchan_conn *c = netchan_open(0);        /* 0 = client, 1 = server */
netchan_connect(c, &server_addr);

struct netchan_chan *ch = netchan_chan_open(c, NETCHAN_RELIABLE,
                                            NETCHAN_DIR_SEND, "state");
netchan_chan_write(ch, msg, len);

for (;;) {
    uint8_t pkt[1500];
    struct nc_addr to;
    size_t n;
    while ((n = netchan_send_next(c, pkt, sizeof pkt, &to)) > 0)
        send_it_however_you_like(pkt, n, &to);

    int wait_ms = netchan_service(c, now_ms());
    /* ... wait for readability, up to wait_ms ... */

    if (got_a_datagram)
        netchan_feed(c, rx, rxlen, &from);

    struct netchan_event ev;
    while (netchan_poll(c, &ev))
        if (ev.type == NETCHAN_EV_DATA)
            netchan_chan_read(ev.ch, buf, sizeof buf);
}
```

Channels come in three flavours. `NETCHAN_RELIABLE` gives ordered, acked,
retransmitted datagrams. `NETCHAN_UNRELIABLE` is fire and forget, which is
what a position update wants. `NETCHAN_STREAM` is a reliable byte stream.
Several of them share one connection and one socket.

### Adding Encryption

`nc_crypto` sits between the socket and `netchan_feed()`. Seal on the way out,
open on the way in, and hand what comes back to the core:

```c
struct nc_crypto cr;
nc_crypto_init(&cr, /*role=*/0, &(struct nc_crypto_cfg){0});

/* Until nc_crypto_ready(), send nc_crypto_handshake_packet() instead of
 * netchan's own output. The crypto handshake finishes first, so even
 * netchan's SYN travels sealed. */
long n = nc_crypto_seal(&cr, pkt, len, sealed, sizeof sealed);
long m = nc_crypto_open(&cr, rx, rxlen, plain, sizeof plain);
if (m > 0) netchan_feed(c, plain, m, &from);
```

Give the responder a long-term `static_sk` and the handshake authenticates it
too. A second Diffie-Hellman folds the identity key into the derivation, so
the first packet that opens is proof of possession. There is no signature on
the wire and no extra round trip. In Noise's naming that is NX, and a client
that compares the presented key against one it recorded earlier is applying
NK's verification to NX's message flow, which is what trust on first use
means.

Deciding *whether* to trust a key is deliberately not this layer's job.
Supply a `verify_peer` callback and answer it however the application wants.

### Adding a Login

`nc_auth` is a pair of state machines with no socket, no netchan, and no event
loop in them. Messages go in, messages come out, and the caller decides what
carries them. `keystore` reads and writes the five file formats:
`known_hosts`, `host_key`, `authorized_keys`, `passwd`, and the client key
file. All are plain text with hex fields, so an operator can read, diff, and
edit them.

The client's signature covers the `nc_crypto` session id, which is what stops
a signature captured by one server being replayed at another.

Wiring the two onto a live connection is application work.
`examples/auth/auth_link.c` is one way to do it.

## Examples

| Program | What it shows |
|---|---|
| `netchan_example` | two peers over plain UDP; the smallest complete thing |
| `ws_gateway` | relays browser WebSocket clients onto an unmodified UDP server |
| `echo_server`, `echo_client` | an encrypted session, no login |
| `auth_server`, `auth_client`, `nc_keygen` | host keys, `known_hosts`, and an ssh-shaped login |

See [`examples/README.md`](examples/README.md) for how to run them, including
generating a key, enrolling it, and watching the host-key warning fire.

## Portability

C11 plus two POSIX calls in the core: `clock_gettime(CLOCK_MONOTONIC)` for its
timers, and a read of `/dev/urandom` to pick a connection id. Build with
`-D_POSIX_C_SOURCE=200809L` under strict `-std=c11`. Nothing else in `src/`
reaches past the C library.

`src/`, `transport/nc_ws.c`, and their tests compile to WebAssembly unchanged;
build with `CC=emcc CXX=em++` to check. `nc_udp`, `nc_crypto`, and `auth/` drop
out of a wasm build automatically, because a browser has no BSD sockets and
its transports are already encrypted.

`nc_crypto` and `keystore` draw randomness from the OS and currently expect
`getrandom(2)` or `/dev/urandom`.

## Status

The protocol is stable enough to build on and is not frozen. Wire
compatibility holds within a minor version and no further; see
[CHANGELOG.md](CHANGELOG.md). The one break so far is `nc_crypto`'s HELLO
growing from 33 to 65 bytes when identity keys arrived.

## Provenance

netchan grew out of a series of research articles. Those articles are the
explanation; this repository is the maintained library.

## Licence

Made by a machine. PUBLIC DOMAIN (CC0-1.0). See [LICENSE](LICENSE).

Vendored monocypher is dual CC0-1.0 / BSD-2-Clause, and the vendored event
loop under `examples/iox/` is MIT-0 or public domain. Both are recorded in
[VENDORING.md](VENDORING.md).
