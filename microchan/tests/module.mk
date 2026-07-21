# microchan tests.
#
#   mc_memlink       an in-memory datagram link: synchronous delivery, and
#                    loss, duplication, and reordering on a fixed count
#   test_microchan   the core protocol driven over that link
#   test_mc_udp      the host UDP transport over real loopback sockets
#
# The core never touches a socket, so the protocol tests do not use one.
# That keeps them deterministic on every platform and lets them impair
# traffic deliberately. test_mc_udp is where the socket work is checked.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

ifneq ($(_TARGET_OS),Emscripten)

LIBRARIES += mc_memlink
mc_memlink_DIR := $(ROOT)
mc_memlink_SRCS = mc_memlink.c
mc_memlink_LIBS = microchan_core
mc_memlink_EXPORTED_CPPFLAGS = -I$(mc_memlink_DIR)

EXECUTABLES += test_microchan
test_microchan_DIR := $(ROOT)
test_microchan_SRCS = test_microchan.c
test_microchan_LIBS = microchan_core mc_memlink
define test_microchan_TESTCMD
$(test_microchan_RUN)
endef
TEST_TARGETS += test_microchan

EXECUTABLES += test_mc_udp
test_mc_udp_DIR := $(ROOT)
test_mc_udp_SRCS = test_mc_udp.c
test_mc_udp_LIBS = mc_udp
define test_mc_udp_TESTCMD
$(test_mc_udp_RUN)
endef
TEST_TARGETS += test_mc_udp

endif
