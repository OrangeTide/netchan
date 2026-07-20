# echo -- an encrypted session with no login.
#
# secure_link is the glue: a netchan connection, an nc_crypto decorator under
# it, and an iox loop that owns the socket, the retransmit timer, and the
# signals. The crypto handshake always finishes before netchan's own, so even
# netchan's SYN travels sealed. That ordering is the reason nc_crypto can stay
# a decorator the protocol core knows nothing about.
#
#   echo_server --port 9000
#   echo_client --port 9000

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

ifneq ($(_TARGET_OS),Emscripten)

LIBRARIES += secure_link
secure_link_DIR := $(ROOT)
secure_link_SRCS = secure_link.c
secure_link_LIBS = netchan_core nc_udp nc_crypto iox
secure_link_EXPORTED_CPPFLAGS = -I$(secure_link_DIR)

SL_LIBS = secure_link netchan_core nc_udp nc_crypto iox monocypher

EXECUTABLES += echo_server
echo_server_DIR := $(ROOT)
echo_server_SRCS = echo_server.c
echo_server_LIBS = $(SL_LIBS)

EXECUTABLES += echo_client
echo_client_DIR := $(ROOT)
echo_client_SRCS = echo_client.c
echo_client_LIBS = $(SL_LIBS)

# Client and server on one loop over real loopback sockets.
EXECUTABLES += test_secure_link
test_secure_link_DIR := $(ROOT)
test_secure_link_SRCS = test_secure_link.c
test_secure_link_LIBS = $(SL_LIBS)
define test_secure_link_TESTCMD
$(test_secure_link_RUN)
endef
TEST_TARGETS += test_secure_link

endif
