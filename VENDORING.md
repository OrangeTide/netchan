# Vendoring netchan

netchan is built to be copied into another project's tree rather than
installed. There is no shared library to ship, no `pkg-config` file, and no
version to negotiate at runtime. You take the source, you own the copy, and
you upgrade when you decide to.

## Getting a Release

Releases are git tags of the form `vMAJOR.MINOR.PATCH`, and GitHub serves a
source snapshot for every tag at a fixed URL. The tag is the whole release
mechanism, and GitHub builds the archive on demand:

```
https://github.com/OrangeTide/netchan/archive/refs/tags/<tag>.tar.gz
```

`tools/vendor.sh` fetches exactly that, takes the layers you name plus the
ones they depend on, and writes a `VENDORED.md` recording the version so the
next upgrade knows where it started. With no `--version` it takes the latest
release:

```sh
curl -fsSL https://raw.githubusercontent.com/OrangeTide/netchan/main/tools/vendor.sh \
  | sh -s -- --layer auth --dest third_party/netchan
```

Run it with `--help` for the options, or `--list` to see what each layer
copies. It needs `curl` or `wget`, and `tar`. It refuses to overwrite an
existing destination unless you pass `--force`, and `--force` first removes
the layers the previous run recorded, so switching layer sets does not leave
stale sources behind.

For a repeatable build, name the release and take the script from that same
tag rather than from `main`. A tag names an immutable commit, so the script
you run is the one that shipped with the release and cannot change under you.
Piping anything from the network into a shell means trusting the server and
the connection; reading it first is the better habit:

```sh
TAG=v0.5.0    # whichever release you want
curl -fsSLO "https://raw.githubusercontent.com/OrangeTide/netchan/$TAG/tools/vendor.sh"
less vendor.sh && sh vendor.sh --version "$TAG" --layer auth
```

None of this needs the script. It copies directories; doing it by hand is a
`tar -xzf` and a `cp -R` of the directories in the table.

`NETCHAN_VENDOR_REPO`, `NETCHAN_VENDOR_BASE_URL`, and `NETCHAN_VENDOR_API_URL`
override where it fetches from, for a fork, a mirror, or an air-gapped copy.

## What to Copy

Take the layer you need and everything below it. Nothing above it is
required.

| You want | Copy | Adds |
|---|---|---|
| the protocol only | `src/` | nothing |
| protocol over UDP | `src/`, `transport/nc_udp.*` | `<sys/socket.h>`, or winsock2 |
| a browser gateway | add `transport/nc_ws.*` | nothing |
| encryption | add `crypto/`, `third_party/` | monocypher, `-lbcrypt` on Windows |
| a login | add `auth/` | monocypher, `-lbcrypt` on Windows |

Every layer here builds on POSIX and on Windows. The core is the only one
that adds nothing to the link line on either, which is deliberate: it draws
its connection ids from `rand_s` in the C runtime rather than from the
`BCryptGenRandom` that `crypto/` and `auth/` use for key material. If you take
`crypto/` or `auth/` onto Windows, add `-lbcrypt`.

`src/nc_addr.h` is needed by every backend and belongs with `src/`. Copy
`third_party/LICENCE-monocypher.md` along with the monocypher sources.

`tests/` and `examples/` are not part of the library. Leave them behind, or
take `tests/` and keep running it after each upgrade, which is the better
habit.

## Fitting It to Your Build

Every source file includes its own headers by plain name, so the only thing a
host build system has to supply is `-I` for each directory it took. There are
no generated files and no configure step. The platform differences that do
exist are selected by the compiler's own predefined macros, `_WIN32` and the
BSD and Apple ones, so nothing has to be detected or configured by the build.

If your project also uses modular-make, each directory already carries a
`module.mk` and you can add them to your `SUBDIRS` unchanged. The
`GNUmakefile` at the top of this repository is a vendored copy of modular-make
itself and is not needed by a project that has its own build.

Otherwise, the file list is short enough to name by hand:

```make
NETCHAN_SRCS = src/netchan.c transport/nc_udp.c crypto/nc_crypto.c \
               third_party/monocypher.c
NETCHAN_CPPFLAGS = -Isrc -Itransport -Icrypto -Ithird_party
```

`Makefile.simple` in this repository does exactly that for the core, in eight
lines, and is kept working as a standing check that the claim holds.

## Upgrading

The layers are independent, but the versions are not: `nc_crypto`,
`nc_auth`, and `keystore` are developed against the core in the same tree.
Upgrade the whole set together, then run `tests/`.

Re-running `tools/vendor.sh --force` with a newer `--version` is the whole
upgrade. The `VENDORED.md` it leaves behind names the version you are on, and
`NETCHAN_VERSION_STRING` in `src/netchan.h` says the same thing, so a copy can
always identify itself:

```c
#if NETCHAN_VERSION < 500          /* 0.5.0 */
#  error "netchan 0.5.0 or newer is required"
#endif
```

Versions are SemVer. The major version is the compatibility promise for the
API; the wire protocol is versioned separately and is stricter.

Wire compatibility holds within a minor version and no further.
[CHANGELOG.md](CHANGELOG.md) records every break with the reason for it, and
there is no negotiation on the wire, so a version skew between two peers shows
up as a handshake that never completes rather than as a subtle failure later.

## Third-Party Code in This Repository

Two things here were written elsewhere. Both are vendored rather than
depended on, for the same reason netchan expects to be vendored itself.

### Monocypher (`third_party/`)

- **Files:** `monocypher.c`, `monocypher.h`
- **Version:** 4.0.2
- **Upstream:** <https://monocypher.org>
- **What it supplies:** X25519 and XChaCha20-Poly1305 for `nc_crypto`,
  Ed25519 for the login signature, BLAKE2b for both key derivations, and
  Argon2id for stretching passwords and key-file passphrases.
- **Why it and not libsodium:** it is public domain, it is one translation
  unit, and it has no build system of its own. For a library that expects to
  be copied into someone else's tree, that is the whole argument.
- **Licence:** dual CC0-1.0 / BSD-2-Clause. See
  `third_party/LICENCE-monocypher.md`.
- **Local changes:** none.

### iox (`examples/iox/`)

- **Files:** `iox_loop.c`, `iox_fd.c`, `iox_signal.c`, `iox_timer.c`, their
  headers, and the header-only priority queue `pq.h`.
- **What it is:** a compact `poll()`-based event loop with file-descriptor
  watchers, one-shot timers on a binary heap, and signal delivery over a
  self-pipe. It is the same event loop the *lumi* terminal workspace uses.
- **Why it is under `examples/`:** the library does not have an event loop and
  does not want one. `netchan_service()` returns the number of milliseconds
  until it next needs attention, which is exactly what every loop wants to be
  told, so netchan fits under whichever one you already run. The example
  programs needed *something*, and this is what they picked.
- **Licence:** MIT-0 OR Public Domain (the headers carry the notice).
- **Local changes:** none.

### modular-make (`GNUmakefile`)

- **Version:** 1.8.5
- **What it is:** a modular multi-target build driver. The `module.mk` files
  throughout the tree are its input.
- **Why vendored:** so a bare checkout builds with nothing but GNU make 4.0
  and a C compiler.
- **Local changes:** none.

## Licence of netchan Itself

Made by a machine. PUBLIC DOMAIN (CC0-1.0). Copy it, change it, ship it,
relicense it, no attribution required.
