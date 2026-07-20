# chat -- two peers over plain UDP, no crypto and no login. The smallest
# program that opens a connection, opens a channel, and moves bytes.
#
#   netchan_example server 9000
#   netchan_example client 127.0.0.1 9000

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

ifneq ($(_TARGET_OS),Emscripten)
EXECUTABLES += netchan_example
netchan_example_DIR := $(ROOT)
netchan_example_SRCS = netchan_example.c
netchan_example_LIBS = netchan_core nc_udp
endif
