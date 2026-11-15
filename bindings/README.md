# netchan language bindings

Optional bindings that let a program drive netchan from Lua or Free Pascal
instead of C. Both are off by default and each needs a toolchain the core
build does not, so neither is built unless you ask for it.

```
make NETCHAN_LUA=1        build the Lua C module
make NETCHAN_PASCAL=1     build and run the Free Pascal demo
make NETCHAN_LUA=1 NETCHAN_PASCAL=1 run-tests   build both and run the smoke tests
```

## The shim

Both bindings sit on one small C layer, `netchan_bind.c`, rather than on the
libraries directly. It wraps every object netchan exposes by value in an
opaque pointer it allocates and frees, reaches each field through a named
accessor, and routes each C callback through a stored function pointer. A
binding written against `netchan_bind.h` never sees a netchan struct layout,
so a field added upstream changes the shim and nothing else. The shim prefix
is `ncb_`.

Return values follow the library being wrapped. A call that returns a byte
count or a netchan status code passes it straight through, so the binding
sees the same numbers the C headers document. Where the shim itself can fail
it returns `NCB_OK` or `NCB_ERR`.

## What the bindings cover

The full surface of `netchan_bind.h`: the protocol core (connections,
channels, events, statistics, configuration, and addresses built from a host
and port), the encrypted transport (`nc_crypto`), service discovery
(`nc_beacon`), the key and password files (`keystore`), and the login
conversation (`nc_auth`, both sides, including forms and stream framing).

The application still owns its socket. The core API takes datagrams in
through `feed` and hands them out through `send_next`, exactly as it does
from C. The examples show this with two connections wired together in one
process, so they need no socket at all.

## Position-independent code

A Lua C module is a shared object and a Free Pascal program is linked as a
position-independent executable. Both need the library objects compiled with
`-fPIC`, which the build turns on for the whole tree only when a binding is
enabled. The plain `make` build is left untouched.

Because the switch changes how objects are compiled without changing where
they are written, do not enable a binding on top of an existing plain build
in the same directory. Start from a clean tree, run `make clean-all` first,
or give the binding build its own variant:

```
make NETCHAN_LUA=1 VARIANT=shared run-tests
```

## Lua

The module builds as `libnetchan.so` with a `luaopen_netchan` entry point.
Lua loads it as the module `netchan` when `package.cpath` carries a
`lib?.so` pattern pointing at the build output:

```lua
package.cpath = "_out/<triplet>/lib/lib?.so;" .. package.cpath
local nc = require("netchan")
```

See `lua/example.lua` for a walkthrough and `lua/test.lua` for the smoke
test. The Lua package name defaults to `lua5.1`; point it at the host's
pkg-config name with `make NETCHAN_LUA=1 LUA_PKG=lua5.4`.

## Free Pascal

`pascal/netchan.pas` is the unit a program uses. It declares the shim's
functions as `cdecl` externals, so a Pascal program that links the netchan
archives can call them directly. `pascal/nc_demo.pas` is the example and the
smoke test in one. The build compiles it with `fpc`, which does its own
linking, and runs it.
