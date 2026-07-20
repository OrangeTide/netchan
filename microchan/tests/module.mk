# microchan tests -- the core driven over two live loopback UDP sockets:
# the handshake, reliable delivery under induced loss, and unreliable
# delivery that is allowed to disappear.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

ifneq ($(_TARGET_OS),Emscripten)
EXECUTABLES += test_microchan
test_microchan_DIR := $(ROOT)
test_microchan_SRCS = test_microchan.c
test_microchan_LIBS = microchan_core mc_udp
define test_microchan_TESTCMD
$(test_microchan_RUN)
endef
TEST_TARGETS += test_microchan
endif
