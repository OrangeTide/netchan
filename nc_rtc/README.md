# nc_rtc: a WebRTC data-channel gateway

The WebRTC counterpart of the WebSocket gateway in `../` (see the
[demo README](../README.md)). It terminates a browser's **WebRTC data channel**
and relays each datagram to the same unmodified UDP `game_server`, so a browser
player over WebRTC and a terminal player over UDP share one server, exactly the
mixed-transport story the WebSocket gateway tells, over the transport that
actually fits a game: unreliable and unordered, with netchan's own reliability
on top.

This is the one part of the netchan-v2 demo that is **not** plain
modular-make/`cc`. A WebRTC data channel is irreducibly ICE + DTLS + SCTP, which
means real libraries, so this component is native-only, opt-in, and built by its
own `build.sh`. The rest of the demo does not depend on it.

## What's here

- `rtc_gateway.c` -- the gateway. Signaling is a single HTTP `POST /offer`
  returning the answer SDP, because [libpeer](https://github.com/sepfy/libpeer)
  gathers ICE non-trickle (every candidate is already in the answer). Each
  browser gets its own thread (libpeer's `peer_connection_loop` blocks during
  the DTLS handshake) and its own UDP socket toward the game server.
- `rtc_probe.c` -- a browser-equivalent WebRTC client in C (a libpeer offerer),
  so the gateway can be tested without a browser: node has no `WebSocket`, and
  nothing here has an `RTCPeerConnection` either.
- `udp_echo.c` -- a trivial UDP echo server standing in for `game_server` in the
  headless test.
- `play-rtc.html` -- the browser client. It reuses the **same** wasm module as
  the WebSocket build (`../game/web/web_client.js`): `nc_web` only calls
  `Module.wsSend` / `web_client_recv`, so the page swaps an `RTCDataChannel`
  where the WebSocket was and nothing above the transport changes.
- `vendor/` -- trimmed source of libpeer and its dependencies (see Licenses).

## Prerequisites

- `cmake` (to build mbedtls, usrsctp and libsrtp) and a C compiler with
  pthreads. Linux `getrandom(2)` is used for ICE credentials.

## Build and test

```sh
./build.sh              # builds the vendored deps, libpeer, and the binaries
./nc_rtc_e2e_test.sh    # headless proof: a datagram relayed through the gateway
```

`build.sh` builds mbedtls, usrsctp and libsrtp with their own CMake into
`build/dist`, then compiles libpeer and the gateway with `cc`. The e2e test
starts the echo server and the gateway, runs `rtc_probe`, and checks the
datagram came back the whole way:
`probe -> data channel -> gateway -> UDP -> echo -> UDP -> gateway -> data channel -> probe`.

## Play in a browser

```sh
../game/build-web-client.sh                 # emits ../game/web/web_client.{js,wasm}
build/rtc_gateway 8090 127.0.0.1 9000 ../game/web   # gateway serving the wasm + page
cp play-rtc.html ../game/web/                # place the page next to its wasm
_out/.../game_server 9000                    # the real server (from the main build)
#   open http://localhost:8090/play-rtc.html
```

Run a native `game_play` next to the browser and both are in the same game.

## libpeer patches

Three changes to the vendored libpeer, all in `vendor/libpeer/`:

- `src/utils.c` -- upstream `utils_random_string` calls `srand(time(NULL))` on
  every call, so two ICE credentials generated in the same second are identical.
  It now draws from `getrandom(2)`. Without this, two browsers connecting in the
  same second get the same ICE ufrag and their connectivity checks are misrouted.
- `peer_signaling.c`, `ssl_transport.c` removed, and the `#include` of
  `peer_signaling.h` dropped from `peer.h`. Those implement libpeer's built-in
  MQTT/HTTP signaling, which this gateway replaces with its own POST endpoint;
  removing them drops the cJSON, coreHTTP and coreMQTT dependencies entirely.
- `usrsctp/CMakeLists.txt` -- `cmake_minimum_required` bumped 3.0 -> 3.5 for
  CMake 4.x.

Two more libpeer facts worth knowing, encoded in the code rather than patched:
a PeerConnection's state only advances if an `oniceconnectionstatechange`
callback is registered, and `peer_connection_create_datachannel` needs SCTP
already up (the gateway is the answerer, so the browser opens the channel and
the gateway receives it via `ondatachannel`).

## Licenses

The vendored dependencies keep their own licenses:

- libpeer -- MIT (`vendor/libpeer/LICENSE`)
- Mbed TLS -- Apache-2.0 (`vendor/mbedtls/LICENSE`)
- usrsctp -- BSD-3-Clause (`vendor/usrsctp/LICENSE.md`)
- libsrtp -- BSD-3-Clause (`vendor/libsrtp/LICENSE`)

Our own files (`rtc_gateway.c`, `rtc_probe.c`, `udp_echo.c`, `play-rtc.html`,
`build.sh`, the tests) are `Made by a machine. PUBLIC DOMAIN (CC0-1.0)`.
