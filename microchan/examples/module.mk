# microchan examples.
#
#   mcdemo    a socketless link and smoke test: two connections in one
#             process with the packets handed between them by hand. It needs
#             no transport at all, which is why it is the first thing that
#             builds on a new target.
#   thor/     the four-player game the variant exists for.
#
# ipxtest.c is here too but is not built by this makefile: it drives the
# real-mode IPX driver and belongs to the Open Watcom build in ../makefile.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

SUBDIRS = thor

ifneq ($(_TARGET_OS),Emscripten)
EXECUTABLES += mcdemo
mcdemo_DIR := $(ROOT)
mcdemo_SRCS = mcdemo.c
mcdemo_LIBS = microchan_core
define mcdemo_TESTCMD
$(mcdemo_RUN)
endef
TEST_TARGETS += mcdemo
endif
