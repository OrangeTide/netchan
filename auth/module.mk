# auth -- who the peer is.
#
#   nc_auth    the login conversation as a pair of state machines. It has no
#              socket, no netchan, and no event loop: messages go in, messages
#              come out, and the caller decides what carries them. The client
#              signature covers the nc_crypto session id, which is what stops
#              a signature captured by one server being replayed at another.
#   keystore   the five on-disk formats, all plain text with hex fields:
#              known_hosts, host_key, authorized_keys, passwd, and the client
#              key file (optionally sealed under an Argon2id passphrase).
#
# Wiring these onto a live connection is the application's job. See
# examples/auth/auth_link.c for one way to do it.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

ifneq ($(_TARGET_OS),Emscripten)
LIBRARIES += nc_auth
nc_auth_DIR := $(ROOT)
nc_auth_SRCS = nc_auth.c
nc_auth_LIBS = monocypher
nc_auth_EXPORTED_CPPFLAGS = -I$(nc_auth_DIR)

LIBRARIES += nc_keystore
nc_keystore_DIR := $(ROOT)
nc_keystore_SRCS = keystore.c
nc_keystore_LIBS = monocypher
nc_keystore_EXPORTED_CPPFLAGS = -I$(nc_keystore_DIR)
# Entropy is BCryptGenRandom on Windows; dependents carry it on the link line.
nc_keystore_EXPORTED_LDLIBS.Windows_NT = -lbcrypt
endif
