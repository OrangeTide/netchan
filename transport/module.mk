# transport -- the backends that turn an nc_addr into real bytes on a wire.
#
#   nc_udp   packs IPv4/IPv6 plus port into nc_addr. The only file in the
#            library that includes a socket header, so it is also the only
#            one excluded from a wasm build.
#   nc_ws    a dependency-free WebSocket codec: RFC 6455 handshake and
#            framing, no sockets of its own. The gateway and the native test
#            client drive it; a browser gets the same behaviour built in.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

# --- WebSocket framing/handshake codec (portable, no sockets) ---
LIBRARIES += nc_ws
nc_ws_DIR := $(ROOT)
nc_ws_SRCS = nc_ws.c
nc_ws_EXPORTED_CPPFLAGS = -I$(nc_ws_DIR)

# --- UDP address packing: needs BSD sockets, so not under emscripten ---
ifneq ($(_TARGET_OS),Emscripten)
LIBRARIES += nc_udp
nc_udp_DIR := $(ROOT)
nc_udp_SRCS = nc_udp.c
nc_udp_CPPFLAGS = $(NETCHAN_SRC_INC)
nc_udp_EXPORTED_CPPFLAGS = -I$(nc_udp_DIR) $(NETCHAN_SRC_INC)
endif
