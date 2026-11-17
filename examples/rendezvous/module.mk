# rendezvous -- the whole reachability path in one program.
#
#   rendezvous_demo
#
# Two peers that have never met end up with a netchan session between them, and
# all three sibling layers do their part: nc_stun_* learns each peer's public
# address, nc_roomcode_* writes the host's short, nc_reach_* punches the path
# open, and netchan runs over it. It is one process with a rendezvous socket and
# two peer sockets on the loopback, so it is deterministic and self-contained.
#
# On the loopback there is no NAT, so the punch always succeeds at once; the
# packets on the wire are the ones a real deployment across two routers sends.
# The rendezvous here also answers STUN, which a real one leaves to a public
# STUN server: the library builds and parses STUN on the client side only.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

ifneq ($(_TARGET_OS),Emscripten)
EXECUTABLES += rendezvous_demo
rendezvous_demo_DIR := $(ROOT)
rendezvous_demo_SRCS = rendezvous_demo.c
rendezvous_demo_LIBS = netchan_core nc_udp nc_nat nc_reach

# Deterministic and self-contained: it returns zero once both peers have
# exchanged a message over the punched path, so running it is the smoke test.
define rendezvous_demo_TESTCMD
$(rendezvous_demo_RUN)
endef
TEST_TARGETS += rendezvous_demo
endif
