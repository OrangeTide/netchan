# microchan core -- the protocol, with every buffer fixed at compile time.
#
# One allocation per connection and none after it. A struct microchan is a
# single fixed-size object whose size is decided by the MC_MTU, MC_WINDOW,
# MC_MAX_CHAN, MC_RECVQ, and MC_UNREL_TXQ macros in microchan.h; opening a
# channel or queueing a message allocates nothing. A server keeps an array of
# them and routes with mc_peek_id(). Override the macros with -D to fit the
# target.
#
# Like netchan's core it never names a socket: mc_feed() in, mc_send_next()
# out, and an opaque struct mc_addr it copies but never reads.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

LIBRARIES += microchan_core
microchan_core_DIR := $(ROOT)
microchan_core_SRCS = microchan.c
microchan_core_EXPORTED_CPPFLAGS = -I$(microchan_core_DIR)
