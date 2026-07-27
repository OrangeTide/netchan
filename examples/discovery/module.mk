# discovery -- nc_beacon with real sockets under it.
#
#   beacon_demo serve --name "Ashen Coast"
#   beacon_demo browse
#
# The server broadcasts every few seconds and answers probes; the browser
# probes at startup so it shows something at once, then listens.
#
# The two rules that keep a probe from being a reflector live here rather
# than in the library, because neither is possible without knowing what an
# address is: the source must share a subnet with one of this machine's
# interfaces, and no address is answered more than once a second.
#
# Both halves on one machine with the defaults find nothing, and that is the
# operating system rather than the code: Linux does not deliver a broadcast
# back to the host that sent it. --to points them at a real address instead,
# which is also what makes the smoke test work in a container that drops
# broadcast entirely.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

ifneq ($(_TARGET_OS),Emscripten)
EXECUTABLES += beacon_demo
beacon_demo_DIR := $(ROOT)
beacon_demo_SRCS = beacon_demo.c
beacon_demo_LIBS = nc_beacon

define beacon_demo_TESTCMD
$(SHELL) $(beacon_demo_DIR)smoke_test.sh "$(beacon_demo_RUN)"
endef
TEST_TARGETS += beacon_demo
endif
