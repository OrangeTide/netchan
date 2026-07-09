#!/bin/sh
# build-web.sh : build the WebAssembly game client into web/
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
#
# Requires emscripten (emcc). After building, serve and play with:
#   emrun web/index.html
# (emrun ships with emscripten; any static file server works too.)

set -e
cd "$(dirname "$0")"

emcc -O2 -I. -I.. \
    -sMODULARIZE=1 -sEXPORT_NAME=createGame \
    -sEXPORTED_FUNCTIONS=_main,_web_step -sEXPORTED_RUNTIME_METHODS=ccall \
    web_demo.c plat_web.c render.c game.c rng.c game_wire.c \
    ../netchan.c ../proto.c \
    -o web/web_demo.js

echo "built web/web_demo.js + web/web_demo.wasm"
echo "serve and play:  emrun web/index.html"
