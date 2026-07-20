# microchan -- the small, statically allocated variant.
#
# Same idea as netchan, different trade. Where netchan keeps allocating as
# channels open and messages queue, microchan takes one allocation per
# connection and fixes everything inside it at compile time: a Go-Back-N
# window instead of selective ack, two channel types instead of three, and a
# core that fits in a 16-bit large-model DOS binary next to a whole game. The
# tunables at the top of microchan.h are the dial.
#
# It is a separate library, not a build option. The wire formats differ, the
# APIs differ, and struct mc_addr and struct nc_addr pack their bytes
# differently. Nothing links both.
#
#   src/         the core, no socket headers
#   transport/   mc_udp for the host, mc_ipx for 16-bit DOS
#   tests/       host loopback tests over UDP
#   examples/    a smoke test and the four-player game the variant exists for
#
# The DOS target is built by Open Watcom from ../microchan/makefile, not from
# here: mc_ipx.c is real-mode 16-bit and does not compile on a host.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

# mc_addr.h is microchan's transport seam, needed on both sides of it without
# either side linking the other. Same reasoning as NETCHAN_SRC_INC.
MICROCHAN_SRC_INC := -I$(ROOT)src

SUBDIRS = src transport tests

ifeq ($(NETCHAN_EXAMPLES),1)
SUBDIRS += examples
endif
