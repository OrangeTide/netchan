# reach -- punching a path between two peers that both sit behind a NAT.
#
# The simultaneous-open companion to the nat helpers: once STUN has learned
# each peer's public address and a rendezvous has exchanged them, this derives
# the session both sides share and builds and parses the one punch packet that
# opens the two NAT bindings together.
#
# A sibling of netchan, not a layer of it. It opens no socket and runs no timer,
# for the same reason the core never names one: the application owns the cadence
# of the punches and the clock. It speaks struct nc_addr, so the only dependency
# is the header-only src/nc_addr.h and the library still links against nothing.
#
# This opens a path; it does not make one trustworthy. The netchan handshake and
# its path challenge validate the address afterwards. See docs/content/reach/.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

LIBRARIES += nc_reach
nc_reach_DIR := $(ROOT)
nc_reach_SRCS = nc_reach.c
nc_reach_CPPFLAGS = $(NETCHAN_SRC_INC)
nc_reach_EXPORTED_CPPFLAGS = -I$(nc_reach_DIR) $(NETCHAN_SRC_INC)
