# microchan transports.
#
#   mc_udp   the host backend. It exists so the core and the game can be
#            developed and tested with gcc on a normal machine, which is the
#            only reason this variant is testable at all.
#   mc_ipx   the 16-bit DOS backend: the real-mode IPX driver reached through
#            INT 2Fh AX=7A00h, a pool of pre-posted listen ECBs polled by the
#            caller, and no Event Service Routine. It cannot compile on a
#            host, so it is absent here and built only by ../makefile under
#            Open Watcom.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

ifneq ($(_TARGET_OS),Emscripten)
LIBRARIES += mc_udp
mc_udp_DIR := $(ROOT)
mc_udp_SRCS = mc_udp.c
mc_udp_CPPFLAGS = $(MICROCHAN_SRC_INC)
mc_udp_EXPORTED_CPPFLAGS = -I$(mc_udp_DIR) $(MICROCHAN_SRC_INC)
endif
