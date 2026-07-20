# src -- the netchan protocol core.
#
# Multiplexed reliable and unreliable channels over an unreliable datagram
# transport. It never names a socket: datagrams arrive through netchan_feed()
# and leave through netchan_send_next(), tagged with an opaque struct nc_addr
# the core copies but never interprets. That seam is why the same object file
# serves UDP on a desktop and a WebSocket in a browser.
#
# Depends on nothing but the C library, and compiles for wasm unchanged.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

LIBRARIES += netchan_core
netchan_core_DIR := $(ROOT)
netchan_core_SRCS = netchan.c
netchan_core_EXPORTED_CPPFLAGS = -I$(netchan_core_DIR)
