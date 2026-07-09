# netchan-v2 game demo -- Caves-of-Thor over netchan

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

# --- transport-agnostic simulation core (deterministic on the server) ---
LIBRARIES += game_core
game_core_DIR := $(ROOT)
game_core_SRCS = game.c rng.c game_wire.c
game_core_EXPORTED_CPPFLAGS = -I$(game_core_DIR)

# --- sim + snapshot round-trip test (socketless: runs everywhere) ---
EXECUTABLES += game_wire_test
game_wire_test_DIR := $(ROOT)
game_wire_test_SRCS = game_wire_test.c
game_wire_test_LIBS = game_core
game_wire_test_LDFLAGS.Emscripten = -sWASM_ASYNC_COMPILATION=0
define game_wire_test_TESTCMD
$(game_wire_test_RUN)
endef
TEST_TARGETS += game_wire_test

# --- server and native client over real UDP sockets: native only ---
ifneq ($(_TARGET_OS),Emscripten)
EXECUTABLES += game_server
game_server_DIR := $(ROOT)
game_server_SRCS = game_server.c
game_server_LIBS = game_core netchan_core nc_udp proto

EXECUTABLES += game_client
game_client_DIR := $(ROOT)
game_client_SRCS = game_client.c
game_client_LIBS = game_core netchan_core nc_udp proto

# playable terminal client (twin-stick controls, curses-free ANSI render)
EXECUTABLES += game_play
game_play_DIR := $(ROOT)
game_play_SRCS = game_play.c render.c plat_host.c
game_play_LIBS = game_core netchan_core nc_udp proto

# headless client over the WebSocket gateway: the browser's path, in C, so it
# can be tested without a browser (and proves a WS client shares the server)
EXECUTABLES += ws_client
ws_client_DIR := $(ROOT)
ws_client_SRCS = ws_client.c
ws_client_LIBS = game_core netchan_core nc_ws proto
endif
