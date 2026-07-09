# netchan-v2 demo

The protocol core from the [netchan-v2 article](../index.md) plus a small
multiplayer game, *Caves of Thor*, played over it. The same core drives four
clients that differ only in their transport file:

| client | transport | where it runs |
| --- | --- | --- |
| `game_client` | UDP | terminal, headless/scriptable |
| `game_play` | UDP | terminal, interactive |
| browser (`web_client`) | WebSocket via the gateway | a web page |
| browser (`web_demo`) | in-process loopback | a web page, no server needed |

The point of the WebSocket path is that a browser player and a terminal player
share **one unmodified UDP server** at the same time: the gateway relays each
browser onto the server as an ordinary UDP peer.

## Prerequisites

- A C compiler (`cc`/`clang`/`gcc`) and `make`. That is all you need for the
  native server, the terminal clients, and the gateway.
- [emscripten](https://emscripten.org) (`emcc`) only if you want to build the
  browser clients.

## Build

```sh
make               # native: server, terminal clients, gateway, tests
make run-tests     # the socketless test suite (protocol, crypto, wire, WebSocket)
```

Binaries land in `_out/<triplet>/bin/`. A minimal `cc`-only build is also kept
in `Makefile.simple`.

## Play in the terminal

Two terminals:

```sh
_out/*/bin/game_server            # authoritative server on udp/9000, 5 Hz
_out/*/bin/game_play              # interactive client; arrows move, WASD fire
```

`game_client` is the same client without a UI, for scripting and tests:

```sh
_out/*/bin/game_client 127.0.0.1 9000 2500 2   # host port run_ms move_dir
```

## Play in the browser (through the gateway)

The browser cannot open a UDP socket, so a gateway terminates its WebSocket and
relays the datagrams to the server. Build the client, start the server, start
the gateway (it also serves the page), then open it:

```sh
game/build-web-client.sh                         # emits game/web/web_client.{js,wasm}

_out/*/bin/game_server 9000                       # 1. the server
_out/*/bin/ws_gateway 8080 127.0.0.1 9000 game/web  # 2. gateway + static files
#   open http://localhost:8080/play.html          # 3. the browser client
```

`ws_gateway [ws_port] [game_host] [game_port] [docroot]` gives each browser its
own UDP socket toward the server. Run a `game_play` next to the browser and you
are both in the same game.

The self-contained `web_demo` (server and client together over a loopback, no
gateway, no server process) is built by `game/build-web.sh` and needs no
network. It is the standalone version that runs anywhere static files are
served.

## Verify the whole path without a browser

`node` has no `WebSocket`, so the gateway path is proven from C instead. A
native WebSocket client, `ws_client`, exercises the same handshake and framing
the browser uses. The end-to-end test runs a WebSocket client and a native UDP
client against one server at once:

```sh
game/ws_e2e_test.sh              # PASS => both transports shared one server
```

## Files

- `nc_addr.h` — the opaque transport address the core copies but never reads.
- `netchan.{c,h}` — the protocol core (no socket headers).
- `nc_udp.{c,h}` — the UDP backend; the only native file that knows `sockaddr`.
- `nc_ws.{c,h}` — a dependency-free WebSocket codec (handshake + framing), used
  by the gateway and the native `ws_client`. The browser gets the same behaviour
  from its built-in `WebSocket`.
- `ws_gateway.c` — WebSocket-to-UDP relay, plus a tiny static file server.
- `nc_crypto.{c,h}` + `monocypher.*` — the encrypted UDP decorator (desktop).
- `game/` — the game: deterministic sim (`game.c`, `rng.c`), wire packing
  (`game_wire.c`), the clients, and the browser transport `nc_web.{c,h}`.

## WebRTC

The gateway here speaks WebSocket, which runs over TCP: reliable and ordered
underneath netchan's own reliability. That is the dependency-free choice and it
is fine on a LAN. A WebRTC data channel (unreliable, unordered) is the closer
fit for a game, and it now exists as a second gateway backend with the same
relay shape: [`nc_rtc/`](nc_rtc/README.md). It terminates a browser's data
channel with a vendored WebRTC stack and relays to the same UDP server; the
browser client swaps an `RTCDataChannel` in for the `WebSocket` and nothing above
the transport moves. It is native-only and opt-in (it needs `cmake` and a real
DTLS/SCTP stack), so it lives apart from this `cc`/modular-make core.
