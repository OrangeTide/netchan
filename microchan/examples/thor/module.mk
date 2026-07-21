# thor -- a four-player, server-authoritative game in a 40x25 text screen.
#
# It is the reason the micro variant exists: the whole thing, engine and
# network layer and renderer, has to fit in a 16-bit large-model DOS binary
# and run at a playable rate inside DOSBox. That budget is what forces the
# static allocation and the Go-Back-N window in the core.
#
# The platform layer is the only part that differs between targets:
# plat_host.c is POSIX and ANSI escapes, plat_dos.c is BIOS and the colour
# text buffer at B800:0000. plat_dos.c is built only by ../../makefile.
#
#   thor          host-and-join over UDP; pass 's' to host
#   test_game     headless simulation check, prints ASCII frames
#   test_gnet     the game's wire protocol over an in-memory link

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

ifneq ($(_TARGET_OS),Emscripten)

# The simulation and its PRNG: deterministic, portable, and no I/O at all.
LIBRARIES += thor_game
thor_game_DIR := $(ROOT)
thor_game_SRCS = game.c rng.c
thor_game_EXPORTED_CPPFLAGS = -I$(thor_game_DIR)

# The wire protocol and the multi-peer server glue.
LIBRARIES += thor_net
thor_net_DIR := $(ROOT)
thor_net_SRCS = game_net.c
thor_net_LIBS = thor_game microchan_core
thor_net_EXPORTED_CPPFLAGS = -I$(thor_net_DIR)

EXECUTABLES += thor
thor_DIR := $(ROOT)
thor_SRCS = thor.c render.c plat_host.c
thor_LIBS = thor_net thor_game microchan_core mc_udp

EXECUTABLES += test_game
test_game_DIR := $(ROOT)
test_game_SRCS = test_game.c
test_game_LIBS = thor_game
# It prints frames rather than asserting, so the check is that it runs clean.
define test_game_TESTCMD
$(test_game_RUN) > /dev/null
endef
TEST_TARGETS += test_game

EXECUTABLES += test_gnet
test_gnet_DIR := $(ROOT)
test_gnet_SRCS = test_gnet.c
test_gnet_LIBS = thor_net thor_game microchan_core mc_memlink
define test_gnet_TESTCMD
$(test_gnet_RUN)
endef
TEST_TARGETS += test_gnet

endif
