# nat -- learning your public address and naming it so a friend can type it.
#
# A sibling of netchan rather than a layer of it, like discovery: STUN builds
# one packet and parses one, the room code is a pure codec, and neither opens a
# socket or runs a timer. The protocol core never sees either.
#
# It speaks raw IPv4 and port, not struct nc_addr, so it depends on nothing in
# netchan and can be vendored alone. The application bridges to nc_addr with the
# nc_udp helpers when it connects.
#
# This is address learning, not NAT traversal: a STUN reply tells you your
# public address, it does not open a closed router's port. See
# docs/content/nat/.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

LIBRARIES += nc_nat
nc_nat_DIR := $(ROOT)
nc_nat_SRCS = nc_nat.c
nc_nat_EXPORTED_CPPFLAGS = -I$(nc_nat_DIR)
