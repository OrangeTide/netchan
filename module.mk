# netchan-v2 demo -- modular-make build descriptor (native + WebAssembly)
#
# Native:  make               (builds every target to _out/<triplet>/bin)
#          make run-tests      (runs the socketless test targets)
# Wasm:    make CC=emcc CXX=em++
#          the socket example is dropped automatically; the protocol core
#          and its tests compile to wasm because the core has no sockets.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))
SUBDIRS = game

# --- the transport-agnostic protocol core (no socket headers) ---
LIBRARIES += netchan_core
netchan_core_DIR := $(ROOT)
netchan_core_SRCS = netchan.c
netchan_core_EXPORTED_CPPFLAGS = -I$(netchan_core_DIR)

# --- UDP transport backend (the only code that knows sockaddr) ---
LIBRARIES += nc_udp
nc_udp_DIR := $(ROOT)
nc_udp_SRCS = nc_udp.c
nc_udp_EXPORTED_CPPFLAGS = -I$(nc_udp_DIR)

# --- WebSocket framing/handshake codec (no sockets, no external libs) ---
# Portable enough to compile anywhere, but only the native gateway and test
# client use it, so it is built for native alongside them.
LIBRARIES += nc_ws
nc_ws_DIR := $(ROOT)
nc_ws_SRCS = nc_ws.c
nc_ws_EXPORTED_CPPFLAGS = -I$(nc_ws_DIR)

# --- generated game wire messages (microser IDL) ---
LIBRARIES += proto
proto_DIR := $(ROOT)/tests
proto_SRCS = proto.c
proto_EXPORTED_CPPFLAGS = -I$(proto_DIR)

# proto.c and proto.h are generated from the schema; both land in the
# source dir so a plain "cc" build and the wasm build share them.
$(proto_DIR)proto.c $(proto_DIR)proto.h &: $(proto_DIR)proto.idl $(proto_DIR)microser-gen.sh
	$(proto_DIR)microser-gen.sh $(proto_DIR)proto.idl $(proto_DIR)proto

# --- protocol loopback tests (socketless: build and run everywhere) ---
EXECUTABLES += netchan_test
netchan_test_DIR := $(ROOT)
netchan_test_SRCS = netchan_test.c
netchan_test_LIBS = netchan_core
# Under emscripten, compile the wasm synchronously so `node test.js` loads it
# from the filesystem instead of via fetch() (which node's global fetch breaks).
netchan_test_LDFLAGS.Emscripten = -sWASM_ASYNC_COMPILATION=0
define netchan_test_TESTCMD
$(netchan_test_RUN)
endef
TEST_TARGETS += netchan_test

# --- WebSocket codec test: RFC 6455 known-answer + frame round-trip ---
# Socketless, so it builds and runs under native and wasm alike.
EXECUTABLES += nc_ws_test
nc_ws_test_DIR := $(ROOT)
nc_ws_test_SRCS = nc_ws_test.c
nc_ws_test_LIBS = nc_ws
nc_ws_test_LDFLAGS.Emscripten = -sWASM_ASYNC_COMPILATION=0
define nc_ws_test_TESTCMD
$(nc_ws_test_RUN)
endef
TEST_TARGETS += nc_ws_test

# --- IDL round-trip test (socketless) ---
EXECUTABLES += proto_test
proto_test_DIR := $(ROOT)/tests
proto_test_SRCS = proto_test.c
proto_test_LIBS = proto
proto_test_LDFLAGS.Emscripten = -sWASM_ASYNC_COMPILATION=0
define proto_test_TESTCMD
$(proto_test_RUN)
endef
TEST_TARGETS += proto_test

# --- chat example over real UDP sockets: native only ---
# Emscripten has no BSD sockets, so this target is excluded there. That
# it can be excluded without touching the core is the whole point of the
# nc_addr transport seam.
ifneq ($(_TARGET_OS),Emscripten)
EXECUTABLES += netchan_example
netchan_example_DIR := $(ROOT)
netchan_example_SRCS = netchan_example.c
netchan_example_LIBS = netchan_core nc_udp

# real-socket test: drives nc_udp over two live loopback UDP sockets
EXECUTABLES += nc_udp_test
nc_udp_test_DIR := $(ROOT)
nc_udp_test_SRCS = nc_udp_test.c
nc_udp_test_LIBS = netchan_core nc_udp
define nc_udp_test_TESTCMD
$(nc_udp_test_RUN)
endef
TEST_TARGETS += nc_udp_test

# browser-to-UDP gateway: terminates WebSocket clients and relays them onto
# the unmodified UDP game server as ordinary peers
EXECUTABLES += ws_gateway
ws_gateway_DIR := $(ROOT)
ws_gateway_SRCS = ws_gateway.c
ws_gateway_LIBS = nc_ws
endif

# --- encrypted UDP transport decorator: desktop only ---
# Browsers get transport encryption for free (WebRTC DTLS, wss), so the
# crypto backend never compiles into the wasm build.
ifneq ($(_TARGET_OS),Emscripten)
LIBRARIES += nc_crypto
nc_crypto_DIR := $(ROOT)
nc_crypto_SRCS = nc_crypto.c monocypher.c
nc_crypto_EXPORTED_CPPFLAGS = -I$(nc_crypto_DIR)

EXECUTABLES += nc_crypto_test
nc_crypto_test_DIR := $(ROOT)
nc_crypto_test_SRCS = nc_crypto_test.c
nc_crypto_test_LIBS = netchan_core nc_crypto
define nc_crypto_test_TESTCMD
$(nc_crypto_test_RUN)
endef
TEST_TARGETS += nc_crypto_test
endif
