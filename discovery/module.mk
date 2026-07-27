# discovery -- nc_beacon, the packet that says a server is here.
#
# A sibling of netchan rather than a layer of it: no handshake, no channels,
# no ordering, no peer. One packet built, one packet parsed, its own magic and
# its own port, so the protocol core never sees one.
#
# It opens no socket and runs no timer, for the same reason the core never
# names one. Broadcasting it, joining a multicast group, answering probes only
# from the local link, and rate-limiting those answers are all the
# application's, because none of them are possible without knowing what an
# address is.
#
# Nothing in a beacon is authenticated and nothing can be. See
# docs/content/discovery/ for why a signature would not fix that, and use
# HTTPS where server authenticity matters.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

LIBRARIES += nc_beacon
nc_beacon_DIR := $(ROOT)
nc_beacon_SRCS = nc_beacon.c
nc_beacon_CPPFLAGS = $(NETCHAN_IDL_INC)
nc_beacon_EXPORTED_CPPFLAGS = -I$(nc_beacon_DIR)
