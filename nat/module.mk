# nat -- learning your public address and naming it so a friend can type it.
#
# The addressing companion to nc_udp: STUN builds one packet and parses one,
# the room code is a pure codec, and neither opens a socket or runs a timer.
# Both speak struct nc_addr, the address type the rest of netchan trades in, so
# the only dependency is the header-only src/nc_addr.h and the library still
# links against nothing. An address STUN learns or a room code decodes drops
# straight into netchan_connect or a CONNECT_REDIRECT frame.
#
# This is address learning, not NAT traversal: a STUN reply tells you your
# public address, it does not open a closed router's port. See
# docs/content/nat/.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

LIBRARIES += nc_nat
nc_nat_DIR := $(ROOT)
nc_nat_SRCS = nc_nat.c
nc_nat_CPPFLAGS = $(NETCHAN_SRC_INC)
nc_nat_EXPORTED_CPPFLAGS = -I$(nc_nat_DIR) $(NETCHAN_SRC_INC)
