# iox -- vendored event loop: poll() fd watchers, one-shot timers on a binary
# heap, and signal delivery over a self-pipe. Self-contained apart from the
# header-only priority queue (pq.h) it bundles for the timer heap.
#
# It is here because the example programs need a loop, not because netchan
# does. See ../../VENDORING.md.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

ifneq ($(_TARGET_OS),Emscripten)
LIBRARIES += iox
iox_DIR := $(ROOT)
iox_SRCS = iox_loop.c iox_fd.c iox_signal.c iox_timer.c
iox_EXPORTED_CPPFLAGS = -I$(iox_DIR)
endif
