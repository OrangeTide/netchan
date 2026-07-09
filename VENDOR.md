# Vendored files

A few files in this demo are copied in from other parts of the project so
the demo builds standalone. They are unmodified.

| File | Origin | Purpose |
|---|---|---|
| `microser-gen.sh` | `serialization-formats/demo/` | IDL compiler (awk); turns `proto.idl` into `proto.h`/`proto.c` |
| `microser.h` | `serialization-formats/demo/` | runtime for the generated encode/decode (no heap, zero-copy strings) |
| `GNUmakefile` | `~/DEVEL/modular-make` (v1.6.0) | modular multi-target build driver (native + wasm) |
| `monocypher.c` / `monocypher.h` | [Monocypher](https://monocypher.org) 4.0.2 | public-domain crypto (X25519, XChaCha20-Poly1305) for the desktop encrypted-UDP backend; see `LICENCE-monocypher.md` |

`proto.h` and `proto.c` are generated from `proto.idl` by `microser-gen.sh`.
They are checked in for convenience and marked "do not edit"; regenerate with:

    ./microser-gen.sh proto.idl proto

All files here are: Made by a machine. PUBLIC DOMAIN (CC0-1.0).
