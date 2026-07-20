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

# modular-make defaults ARFLAGS to "rvD". The D asks for a deterministic
# archive and is GNU binutils only; Apple's ar rejects it outright. The
# archive rule sends stderr to /dev/null, so on a Mac the whole build stops
# with a bare "Error 1" and no hint as to why.
#
# Probe for it once, with an empty archive so no compiler is involved, and
# drop the flag when it is not understood. A command-line ARFLAGS still wins
# over this, because the command line beats every makefile assignment.
NETCHAN_AR_HAS_D := $(shell _d=$$(mktemp -d) && \
    $(AR) rcD $$_d/probe.a >/dev/null 2>&1 && echo yes; rm -rf $$_d)
ifneq ($(NETCHAN_AR_HAS_D),yes)
ARFLAGS = rv
endif

SUBDIRS = third_party src transport crypto auth tests

NETCHAN_EXAMPLES ?= 1
ifeq ($(NETCHAN_EXAMPLES),1)
SUBDIRS += examples
endif

# microchan is a second, incompatible library that shares this repository but
# not a line of code. See microchan/README.md.
SUBDIRS += microchan
