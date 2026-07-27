# netchan -- top-level modular-make descriptor.
#
# The library is four layers, each in its own directory and each buildable
# without the ones above it:
#
#   src/         the protocol core. No socket headers, no crypto, no loop.
#   transport/   nc_udp and nc_ws: the only code that knows sockaddr or HTTP.
#   crypto/      nc_crypto, a transport decorator. Needs monocypher.
#   auth/        nc_auth and keystore: the login conversation and its files.
#   idl/         microser: message definitions compiled to structs and codecs.
#
# third_party/ holds vendored monocypher. examples/ and tests/ are not part
# of the library and a vendoring project normally leaves them behind; see
# VENDORING.md.
#
#   make                 build everything for the host
#   make run-tests       run the test targets
#   make lint            check the tree against docs/coding-style.md
#   make analyze         build under gcc -fanalyzer and fail on a warning
#   make NETCHAN_EXAMPLES=0   library and tests only

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

# nc_addr.h is the transport seam: the core copies these bytes and every
# backend packs them, so both sides of the seam need it on the include path
# without either one linking the other. This is the whole reason for a
# project-wide variable; everything else propagates through _LIBS.
NETCHAN_SRC_INC := -I$(ROOT)src

# Every subdirectory's module.mk rebinds ROOT as it is included, so a recipe
# expanded later would see whichever one came last. Capture the top now.
NETCHAN_TOP := $(ROOT)

# idl comes before auth, tests, and examples because it exports
# NETCHAN_IDL_INC and MICROSER_GEN, which they read at include time.
SUBDIRS = third_party src transport crypto idl auth tests

NETCHAN_EXAMPLES ?= 1
ifeq ($(NETCHAN_EXAMPLES),1)
SUBDIRS += examples
endif

# microchan is a second, incompatible library that shares this repository but
# not a line of code. See microchan/README.md.
SUBDIRS += microchan

# Style checking is not a build step: it compiles nothing and produces no
# output file, so it stays a plain phony rule rather than a modular-make
# target. It reads the tree through git ls-files, so it needs no file list
# here that could drift from the one the build uses.
.PHONY : lint
lint :
	@sh $(NETCHAN_TOP)tools/lint.sh

# The analyzer needs its own flags and a clean tree, so it drives make itself
# rather than hooking into this one. Slow, and gcc only.
.PHONY : analyze
analyze :
	@sh $(NETCHAN_TOP)tools/analyze.sh
