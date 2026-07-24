# chat -- two peers over plain UDP, no crypto and no login. The smallest
# program that opens a connection, opens a channel, and moves bytes.
#
#   netchan_example server
#   netchan_example client <name>
#
# The port is fixed at 9900, so the smoke test cannot be run twice at once on
# one machine.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

ifneq ($(_TARGET_OS),Emscripten)
EXECUTABLES += netchan_example
netchan_example_DIR := $(ROOT)
netchan_example_SRCS = netchan_example.c
netchan_example_LIBS = netchan_core nc_udp

# Two processes, a socket, and a signal: the parts a unit test cannot reach.
# The script takes the run command rather than a path so TESTWRAP survives.
define netchan_example_TESTCMD
$(SHELL) $(netchan_example_DIR)smoke_test.sh "$(netchan_example_RUN)"
endef
TEST_TARGETS += netchan_example
endif
