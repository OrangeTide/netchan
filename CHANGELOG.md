# Changelog

Wire compatibility holds within a minor version and no further. There is no
version negotiation on the wire, so a skew between two peers shows up as a
handshake that never completes, not as a subtle failure later.

Releases are SemVer and are tagged `vMAJOR.MINOR.PATCH`. The tag is the
release: GitHub serves a source snapshot for it, which is what
`tools/vendor.sh` fetches.

## 0.8.0 - 2026-07-26

The wire protocol is untouched. Everything here is source level, so a peer
built from this tree still talks to one built from 0.7.0. The minor version
moves because the source API does.

### Added

- `docs/coding-style.md`, the project's written C conventions, linked from the
  README. It is written to drop into another project unchanged.
- `make lint`, which checks the tree against that document: tabs, trailing
  whitespace, non-ASCII, missing final newline, `#pragma once`, per-file
  licence lines in C and in scripts, `extern` on a function declaration, bare
  `-1` returns, lines over 100 columns, a definition with its body on the
  signature line, a missing tag line, and declarations below the first
  statement of a block. It runs as its own CI job, so the conventions hold
  without a manual sweep.
- `make analyze`, a `gcc -fanalyzer` build that fails on a warning in the
  project's own code. The analyzer walks execution paths rather than matching
  patterns, so it catches what a review misses: it found a descriptor leaked
  down a failure branch in the secure-link test and a `bind` on an unchecked
  `socket` result in the UDP test, both fixed here.
- The manual is a multi-page site rather than one assembled HTML page:
  introduction, architecture, protocol overview, four tutorials, message
  encoding, protocol reference, testing and analysis, and the API reference.
  The prose was converted, not rewritten. The API reference is still generated
  from `src/netchan.h`, and `check-symbols.sh` still fails the build when prose
  names a function the header no longer declares, now across every page.
- Every documentation page carries the version it was built from. At a release
  tag that is the bare version; away from one it is the version, the commit
  count, and the abbreviated sha, so a page from an untagged build says which
  commit produced it.
- A fuzz harness for `netchan_feed`, the core's entire untrusted surface, with
  a checked-in corpus. `make run-tests` replays the corpus on every platform,
  which turns any input the fuzzer once crashed on into a regression test, and
  a CI job fuzzes for two minutes on top of that to explore new ground. Half a
  million executions found no crash.

### Changed

- Every layer now names its success and failure returns instead of returning a
  bare `0` and `-1`: `NETCHAN_OK`/`NETCHAN_ERR`, and the same pair under the
  `MC_`, `NC_UDP_`, `NC_CRYPTO_`, `NC_AUTH_`, `KS_` and `MEML_` prefixes. They
  are `#define`, so the return type stays a plain `int` and a caller that tests
  `< 0` without including the header is still correct. The values are
  unchanged. `netchan.h` and `microchan.h` kept their error causes as an enum,
  starting at `-2`.
- `nc_ws` and `microser` name only a failure value, `NC_WS_ERR` and `MS_ERR`.
  Their calls return a position or a length, where `0` is a real answer rather
  than success.
- Functions that answer a question return `bool`: `nc_crypto_ready`,
  `nc_crypto_failed`, `ks_authorized_key`, `ks_check_password`,
  `ks_user_exists`, `ks_keyfile_encrypted`, `mc_ipx_available`,
  `secure_link_up` and `auth_link_up`.
- No file carries a licence line any more, C or shell or awk. The root
  `LICENSE` is the one place the terms live, so relicensing is one edit rather
  than a sweep. The IDL compiler stopped emitting one into generated code too,
  so a regenerated codec matches.
- Declarations sit at the top of the block that uses them. Where the value
  came from a call whose order matters, or from a read a guard clause proves
  is in bounds, the type moved up and the assignment stayed put.
- Doc comments in the public headers open with `/**`, so an API description is
  distinguishable from the prose that explains a file.

### Breaking

- The `nc_auth` conversation states are renamed to `NC_AUTH_STATE_PENDING`,
  `NC_AUTH_STATE_OK` and `NC_AUTH_STATE_DENIED`. `NC_AUTH_OK` now means a call
  succeeded, which is what it means in every other layer. Code that compared
  `nc_auth_state()` against the old names has to follow, and it will not fail
  quietly: the old spellings no longer exist.
- The `nc_auth_server_cb` callbacks `check_key` and `check_password` return
  `bool` rather than `int`. An existing implementation keeps working once its
  own signature changes; the compiler flags the mismatch.
- `auth/keystore.h` guards with `KS_KEYSTORE_H` and `examples/common/sockutil.h`
  with `SU_SOCKUTIL_H`, so a vendored copy cannot collide with a host project's
  own `KEYSTORE_H` or `SOCKUTIL_H`.
- `microchan/tests/mc_memlink.{c,h}` is renamed to `memlink.{c,h}`, matching
  the `meml_` symbols it exports, and its guard is `MEML_MEMLINK_H`. The build
  library is `memlink`. This is test-only and ships to nobody, but a project
  that vendored microchan's tests has a file to rename.

## 0.7.0 - 2026-07-24

### Added

- An `idl/` layer, which brings microser back as a message encoder for a
  channel's payload. `microser.h` is the runtime, a header-only reader and
  writer for tagged fields, and `microser-gen.sh` compiles an `.idl` file into
  a C struct with an encode and a decode function. The core still names neither
  a socket nor a serialiser: netchan moves opaque bytes, and this says what
  they mean.
- A dispatch table generated from the same `.idl`, so a program routes an
  incoming datagram to a typed handler instead of switching on a tag itself. A
  channel's `content_type` is the topic, read back from its OPEN, so a channel
  can be routed before a byte of payload is read.
- A manual, built by `docs/build.sh` into one self-contained page and deployed
  to GitHub Pages on a push to main. The prose is hand written, the API
  reference is generated from `src/netchan.h`, and `check-symbols.sh` fails the
  build if the prose calls a `netchan_` function the header no longer declares.
  Diagrams are rendered to SVG at build time by mscgen and graphviz, with their
  colors swapped for `currentColor` so one file serves light and dark.

### Fixed

- The IDL compiler folded two variant fields sharing a tag number into a single
  struct member, so a union that reused a number generated code that did not
  compile. Every variant field now gets its own member and its own decode case.
  The field-number check also compared only plain fields against each other, so
  a plain field colliding with a variant or with the discriminant reached the C
  compiler as a duplicate case value. The full field set is checked now, and a
  collision is reported by name.
- A generator run with no output base wrote empty `.c` and `.h` files. The
  guard was in `BEGIN`, where `exit` still runs `END`. It is in `END` now,
  where `exit` is terminal.
- The chat server polled a peer it had just freed.

### Changed

- The IDL compiler and the API-reference generator moved out of shell
  single-quoted strings into `microser-gen.awk` and `gen-api.awk`, invoked with
  `awk -f`. Every apostrophe and quote in them was a quoting hazard, and one
  such apostrophe had already spilled part of the program to the shell. As
  ordinary source files they parse-check and pass `gawk --lint`. Generated
  output is unchanged. A vendored copy needs both files of each pair.

## 0.6.0 - 2026-07-23

### Added

- `netchan_disconnect()`: queue a DISCONNECT for the peer and enter CLOSING,
  without freeing. netchan has no socket of its own, so a caller must run one
  `netchan_send_next` cycle to put the frame on the wire, then `netchan_close`
  to free. This is the graceful-shutdown counterpart to `netchan_close`, which
  frees without telling the peer anything. It does nothing unless the session
  is CONNECTED, and the frame is best effort: it goes out once, unreliably,
  and is not retransmitted, so a lost datagram leaves the peer waiting out its
  idle timeout as before.

### Changed

- `netchan_close` no longer queues a DISCONNECT. It wrote the frame into a
  buffer the same call then freed, so nothing ever reached the wire, and the
  dead write made close read like a graceful shutdown.

### Fixed

- A repeated DISCONNECT no longer raises a second `NETCHAN_EV_DISCONNECTED`.
  Nothing in the receive path filters a duplicated or replayed datagram, so
  the frame that ended a session can arrive again and the application saw a
  second disconnect for a connection it had already torn down. Only a session
  that still believes it is live reports the news now. One that disconnected
  locally advances to CLOSED without an event, since its caller ended the
  session itself and is not waiting to be told.

- The transport wrappers closed a live connection with `netchan_close` alone,
  so nothing told the peer the link had ended and it waited out its idle
  timeout, seconds later, to notice. `secure_link_close`, `auth_link_close`,
  and the chat example now call `netchan_disconnect`, flush, then
  `netchan_close`, so a peer sees a clean shutdown at once. A program that
  copied one of these wrappers as its starting point inherited the delay; that
  is exactly how it was found downstream. The two encrypted wrappers skip the
  disconnect when their keys are not ready, since the flush would seal nothing
  and drop the frame. A session only reaches CONNECTED after its crypto
  handshake, so this never skips a disconnect a live link would have sent.

## 0.5.0 - 2026-07-20

The first tagged release. Earlier versions here describe the tree as it was
developed, not as anything anyone could name and fetch.

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
- `nc_random_id()` fell back to an unseeded `rand()` whenever it could not
  read `/dev/urandom`, so every process produced the identical sequence of
  connection ids. That was the normal path on Windows, where the file does not
  exist, and reachable anywhere `/dev` is missing. It now asks the platform
  for real entropy, and the last-resort path mixes the clock with a stack
  address instead of calling `rand()` at all.
- Three tests read from a loopback socket without ever waiting and treated an
  empty queue as no traffic. Linux queues a loopback datagram inside
  `sendto()`, so the read always found it; macOS delivers through a separate
  context, so the reads came back empty and the tests gave up early. That
  failed the macOS CI job in `nc_udp_test` and `test_gnet`, and left
  `test_microchan` passing on a few microseconds of luck. The socket tests now
  wait for the datagram, and the protocol tests no longer use a socket at all.

### Added

- A version, and a way to get one. The project is SemVer from 0.5.0 on, the
  version is in `src/netchan.h` as `NETCHAN_VERSION_MAJOR`, `_MINOR`, `_PATCH`,
  `_STRING`, and a comparable `NETCHAN_VERSION` integer, and releases are git
  tags of the form `v0.5.0`. A release workflow refuses to publish a tag that
  disagrees with the header.
- `tools/vendor.sh`, which copies a release into another project's tree. It
  takes the layers you name plus the ones they depend on, from the source
  snapshot GitHub serves for the tag, and records what it took. It runs
  standalone or piped:

  ```sh
  curl -fsSL https://raw.githubusercontent.com/OrangeTide/netchan/v0.5.0/tools/vendor.sh \
    | sh -s -- --version v0.5.0 --layer auth
  ```
- `tests/netchan_test`: the channel accessors, including a receiver
  identifying channels by the content type carried in the peer's OPEN, and
  the truncation seam where the 64-byte field meets the 63-byte wire cap.
- Windows support for the library. The core takes its monotonic clock from
  `GetTickCount64` and its connection ids from `rand_s`; `nc_crypto` and
  `keystore` take entropy from `BCryptGenRandom`; `nc_udp.h` includes winsock2
  instead of `<sys/socket.h>`. `crypto/` and `auth/` need `-lbcrypt`, which
  the build now exports to their dependents. The core needs nothing on the
  link line, which is why it uses `rand_s` rather than the same call: a
  vendored `src/` still builds with nothing but a compiler.

  The examples are unchanged and remain POSIX. They own sockets, an event
  loop, and a terminal.
- A CI job that cross-compiles for Windows with mingw-w64 and runs every
  socket-free test binary under wine, so the entropy and clock backends are
  executed rather than merely linked. Without it this would rot unnoticed,
  which is exactly what happened to macOS.
- `netchan_chan_content_type()`, the accessor for the string given to
  `netchan_chan_open()` or carried in the peer's OPEN. It tells apart channels
  that share a netchan type, such as two reliable streams named "control" and
  "events". Never NULL; a channel opened without one reads back as "".
- `microchan/tests/mc_memlink`, an in-memory datagram link for tests:
  delivery is a function call, and loss, duplication, and reordering happen on
  a fixed count. `test_microchan` gains a check that the Go-Back-N window
  holds its line through reordering and duplication, which loopback UDP could
  not produce on demand.
- `microchan/tests/test_mc_udp`, covering the host UDP transport on its own:
  the ephemeral bind, the address packing, the return convention of a
  non-blocking recv, and that the address a datagram arrives with is usable as
  a destination.

### Changed

- `test_microchan` and `test_gnet` run over `mc_memlink` instead of loopback
  UDP. Neither the core nor the game net layer touches a socket, so the
  socket was testing nothing about them while making both tests
  platform-dependent. They are now deterministic everywhere, and `test_gnet`
  went from 1.2s to 0.3s.

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
