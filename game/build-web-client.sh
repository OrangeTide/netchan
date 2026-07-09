#!/bin/sh
# build-web-client.sh : build the remote WebAssembly game client into web/
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
#
# This is the client-only browser build that talks to a real server through
# the WebSocket gateway (build-web.sh builds the self-contained loopback demo
# instead). Requires emscripten (emcc). After building:
#
#   ../ws_gateway 8080 127.0.0.1 9000 web   # from the built binary dir
# then open  http://localhost:8080/play.html  in a browser.

set -e
cd "$(dirname "$0")"

emcc -O2 -I. -I.. \
    -sMODULARIZE=1 -sEXPORT_NAME=createGameClient \
    -sEXPORTED_FUNCTIONS=_main,_web_step,_web_client_recv,_nc_web_inbuf,_web_client_player,_web_client_snaps \
    -sEXPORTED_RUNTIME_METHODS=ccall,HEAPU8 \
    web_client.c nc_web.c plat_web.c render.c game.c rng.c game_wire.c \
    ../netchan.c ../proto.c \
    -o web/web_client.js

echo "built web/web_client.js + web/web_client.wasm"
echo "run the gateway, then open http://localhost:8080/play.html"
