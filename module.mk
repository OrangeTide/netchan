# netchan -- top-level modular-make descriptor.
#
# The library is four layers, each in its own directory and each buildable
# without the ones above it:
#
#   src/         the protocol core. No socket headers, no crypto, no loop.
#   transport/   nc_udp and nc_ws: the only code that knows sockaddr or HTTP.
#   crypto/      nc_crypto, a transport decorator. Needs monocypher.
#   auth/        nc_auth and keystore: the login conversation and its files.
#
# third_party/ holds vendored monocypher. examples/ and tests/ are not part
# of the library and a vendoring project normally leaves them behind; see
# VENDORING.md.
#
#   make                 build everything for the host
#   make run-tests       run the test targets
#   make NETCHAN_EXAMPLES=0   library and tests only

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

# nc_addr.h is the transport seam: the core copies these bytes and every
# backend packs them, so both sides of the seam need it on the include path
# without either one linking the other. This is the whole reason for a
# project-wide variable; everything else propagates through _LIBS.
NETCHAN_SRC_INC := -I$(ROOT)src

SUBDIRS = third_party src transport crypto auth tests

NETCHAN_EXAMPLES ?= 1
ifeq ($(NETCHAN_EXAMPLES),1)
SUBDIRS += examples
endif
