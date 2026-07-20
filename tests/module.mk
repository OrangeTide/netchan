# tests -- the library's own test suite.
#
# Most of it is socketless on purpose: the protocol, the WebSocket codec, the
# AEAD decorator, and the login state machine are all driven in one process
# with buffers between the two halves, so they build and run under wasm as
# readily as under gcc. nc_udp_test is the exception and needs real loopback
# sockets, so it is native-only.
#
#   make run-tests

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

# --- protocol core: connect, channels, loss, reorder, flow control ---
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

# --- WebSocket codec: RFC 6455 known-answer plus frame round-trip ---
EXECUTABLES += nc_ws_test
nc_ws_test_DIR := $(ROOT)
nc_ws_test_SRCS = nc_ws_test.c
nc_ws_test_LIBS = nc_ws
nc_ws_test_LDFLAGS.Emscripten = -sWASM_ASYNC_COMPILATION=0
define nc_ws_test_TESTCMD
$(nc_ws_test_RUN)
endef
TEST_TARGETS += nc_ws_test

ifneq ($(_TARGET_OS),Emscripten)

# --- nc_udp over two live loopback sockets ---
EXECUTABLES += nc_udp_test
nc_udp_test_DIR := $(ROOT)
nc_udp_test_SRCS = nc_udp_test.c
nc_udp_test_LIBS = netchan_core nc_udp
define nc_udp_test_TESTCMD
$(nc_udp_test_RUN)
endef
TEST_TARGETS += nc_udp_test

# --- the encrypted decorator: handshake, replay, identity keys ---
EXECUTABLES += nc_crypto_test
nc_crypto_test_DIR := $(ROOT)
nc_crypto_test_SRCS = nc_crypto_test.c
nc_crypto_test_LIBS = netchan_core nc_crypto monocypher
define nc_crypto_test_TESTCMD
$(nc_crypto_test_RUN)
endef
TEST_TARGETS += nc_crypto_test

# --- the login, both halves in one process with a queue between them ---
EXECUTABLES += test_nc_auth
test_nc_auth_DIR := $(ROOT)
test_nc_auth_SRCS = test_nc_auth.c
test_nc_auth_LIBS = nc_auth monocypher
define test_nc_auth_TESTCMD
$(test_nc_auth_RUN)
endef
TEST_TARGETS += test_nc_auth

# --- the five on-disk formats, in a temporary directory ---
EXECUTABLES += test_keystore
test_keystore_DIR := $(ROOT)
test_keystore_SRCS = test_keystore.c
test_keystore_LIBS = nc_keystore monocypher
define test_keystore_TESTCMD
$(test_keystore_RUN)
endef
TEST_TARGETS += test_keystore

endif
