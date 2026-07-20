# third_party -- vendored code. See ../VENDORING.md for provenance.
#
# Monocypher supplies X25519 and XChaCha20-Poly1305 for nc_crypto, Ed25519
# for the login signature, BLAKE2b for both key derivations, and Argon2id for
# stretching passwords and key-file passphrases.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

LIBRARIES += monocypher
monocypher_DIR := $(ROOT)
monocypher_SRCS = monocypher.c
monocypher_EXPORTED_CPPFLAGS = -I$(monocypher_DIR)
