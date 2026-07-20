# Changelog

Wire compatibility holds within a minor version and no further. There is no
version negotiation on the wire, so a skew between two peers shows up as a
handshake that never completes, not as a subtle failure later.

## Unreleased

### Fixed

- The build passed `ar rvD`, whose `D` flag is GNU binutils only, so building
  any archive failed on macOS, and with the archiver's stderr discarded it
  failed with no message. Fixed upstream in modular-make 1.8.5, which probes
  for the flag, adds `c` so the archiver is silent unless something is wrong,
  and no longer discards its stderr. The vendored `GNUmakefile` moves from
  1.8.0 to 1.8.5.
- `nc_crypto` and `keystore` called `getrandom(2)` unconditionally, so
  neither compiled anywhere but Linux. Both now select a backend at compile
  time: `arc4random_buf` on macOS and the BSDs, `getrandom(2)` on Linux with
  a retry on `EINTR`, and `/dev/urandom` otherwise, which also covers a Linux
  kernel too old for the syscall. macOS is now built and tested in CI, which
  is how this was found.

### Added

- `microchan/`, a second library: the same idea with one allocation per
  connection and none after it, every buffer inside it fixed at compile time,
  Go-Back-N over an 8-message window instead of selective ack, and a core that
  fits in a 16-bit large-model DOS binary.
  Transports are IPX for MS-DOS and UDP for a host. It brings its own tests,
  the four-player `thor` game the variant exists for, and an Open Watcom
  `makefile` for the DOS target.

  It is a separate library, not a build option. Its API was renamed on import
  from `nc_*`/`NC_*`/`struct netchan` to `mc_*`/`MC_*`/`struct microchan`,
  because the two trees otherwise both define `struct nc_addr` with different
  layouts and both ship an `nc_addr.h`, `nc_udp.c/h`, and `netchan.c/h`. Same
  names, different meanings, one repository. Nothing links both.

## 0.3.0

The library is reorganised into layers, each in its own directory, and split
from the demo programs that used to sit alongside it.

### Added

- `crypto/nc_crypto`: long-term X25519 identity keys. A side that holds one
  puts its public half in the HELLO, and a second Diffie-Hellman folds it into
  the key derivation, so the first packet that opens is proof of possession.
  No signature on the wire, no extra round trip.
- `crypto/nc_crypto`: a `verify_peer` callback, called once with the peer's
  static key before any key material is derived from it. Deciding whether to
  trust a key is the application's business, not the transport's.
- `crypto/nc_crypto`: `nc_crypto_session_id()`, a 32-byte value derived from
  the same transcript as the session keys under a different label. Higher
  layers sign it to bind their authentication to one exact session.
- `auth/nc_auth`: the login conversation as a pair of state machines, with no
  socket, no netchan, and no event loop in them. Public key with a password
  fallback.
- `auth/keystore`: the five on-disk formats -- `known_hosts`, `host_key`,
  `authorized_keys`, `passwd`, and the client key file. All plain text with
  hex fields, and the key file can be sealed under an Argon2id passphrase.
- `tests/nc_crypto_test`: coverage for identity keys, for a refused peer
  sealing nothing, and for session ids differing between sessions.
- `VENDORING.md`, `CHANGELOG.md`, `LICENSE`.

### Changed

- **Wire break.** `nc_crypto`'s HELLO grew from 33 to 65 bytes: the static
  public key field is always present, zero-filled when the sender has no
  identity key, so both HELLOs are the same size and a spoofed source gets no
  amplification to abuse. A 0.3 peer cannot talk to a 0.2 peer.
- **API break.** `nc_crypto_init()` takes a `struct nc_crypto_cfg *` instead
  of two pointer arguments, so future knobs do not keep changing its
  signature. `nc_crypto_init(c, role, seed, psk)` becomes
  `nc_crypto_init(c, role, &(struct nc_crypto_cfg){ .eph_sk_seed = seed, .psk = psk })`,
  and `NULL` still selects a fresh ephemeral key and no pre-shared key.
- Sources moved into `src/`, `transport/`, `crypto/`, `auth/`,
  `third_party/`, `tests/`, and `examples/`. Includes are unchanged: every
  file still names its headers plainly and the build supplies the `-I`.
- The vendored modular-make `GNUmakefile` moved to 1.8.0.

### Removed

- The *Caves of Thor* game demo and the microser IDL it used. Both exercised
  the API rather than being part of it.
- The `nc_rtc` WebRTC gateway, which vendored mbedtls, libsrtp, and libpeer.
  It needs `cmake` and a real DTLS/SCTP stack, which is the opposite of what
  this repository is for.

## 0.2.0

- `crypto/nc_crypto`: the encrypted transport decorator. X25519 handshake,
  XChaCha20-Poly1305 per packet with a 64-bit counter nonce, and a sliding
  replay window on receive. It wraps datagrams on their way past, so the
  protocol core is not aware it exists.
- `transport/nc_ws`: a dependency-free WebSocket codec, handshake and framing,
  with no sockets of its own.
- `ws_gateway`: relays browser WebSocket clients onto an unmodified UDP server
  as ordinary peers, so a browser player and a native player share one server.

## 0.1.0

- The protocol core: multiplexed reliable, unreliable, and stream channels
  over an unreliable datagram transport, with connection migration, flow
  control, and no socket headers anywhere in it.
- `transport/nc_udp`: IPv4 and IPv6 packing into `nc_addr`, and the only file
  that names `sockaddr`.
