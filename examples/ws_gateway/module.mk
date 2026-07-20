# ws_gateway -- terminates browser WebSocket clients and relays each onto an
# unmodified UDP server as an ordinary peer, so a browser player and a native
# player share one server. Also serves static files, to save running a second
# thing during development.
#
#   ws_gateway [ws_port] [game_host] [game_port] [docroot]

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

ifneq ($(_TARGET_OS),Emscripten)
EXECUTABLES += ws_gateway
ws_gateway_DIR := $(ROOT)
ws_gateway_SRCS = ws_gateway.c
ws_gateway_LIBS = nc_ws
endif
