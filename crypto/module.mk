# crypto -- nc_crypto, the encrypted transport decorator.
#
# Sits between the application's socket and netchan_feed()/netchan_send_next():
# an X25519 handshake, XChaCha20-Poly1305 on every packet, a replay window,
# and optional long-term identity keys with a verify_peer callback. The
# protocol core is not aware any of it happened.
#
# Browsers already encrypt their transports (WebRTC DTLS, wss), so this layer
# is desktop-only and is left out of a wasm build.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

ifneq ($(_TARGET_OS),Emscripten)
LIBRARIES += nc_crypto
nc_crypto_DIR := $(ROOT)
nc_crypto_SRCS = nc_crypto.c
nc_crypto_CPPFLAGS = $(NETCHAN_SRC_INC)
nc_crypto_LIBS = monocypher
nc_crypto_EXPORTED_CPPFLAGS = -I$(nc_crypto_DIR) $(NETCHAN_SRC_INC)
# Entropy is BCryptGenRandom on Windows; dependents carry it on the link line.
nc_crypto_EXPORTED_LDLIBS.Windows_NT = -lbcrypt
endif
