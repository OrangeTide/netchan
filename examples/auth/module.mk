# auth -- the whole stack, with host keys and an ssh-shaped login.
#
# Four layers, with the event loop underneath all of them:
#
#   nc_auth      who the client is           (messages on a reliable channel)
#   netchan      reliable ordered delivery   (the protocol core)
#   nc_crypto    secrecy and server identity (transport decorator)
#   iox          socket readiness, timers, signals
#
# auth_link is the only file that holds all four in view, and it is small
# because the seams are real. The order is fixed: crypto handshake, then
# netchan connect over the sealed transport, then the login on a reliable
# channel with its signature bound to the crypto session id, then application
# bytes. Never earlier.
#
#   nc_keygen -f id_netchan       make a client identity
#   auth_server --adduser bob     enrol a password
#   auth_server --port 9000
#   auth_client --port 9000 --user alice

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

ifneq ($(_TARGET_OS),Emscripten)

LIBRARIES += auth_link
auth_link_DIR := $(ROOT)
auth_link_SRCS = auth_link.c
auth_link_LIBS = netchan_core nc_udp nc_crypto nc_auth iox
auth_link_EXPORTED_CPPFLAGS = -I$(auth_link_DIR)

AL_LIBS = auth_link netchan_core nc_udp nc_crypto nc_auth nc_keystore \
          demoutil iox monocypher

EXECUTABLES += auth_server
auth_server_DIR := $(ROOT)
auth_server_SRCS = auth_server.c
auth_server_LIBS = $(AL_LIBS)

EXECUTABLES += auth_client
auth_client_DIR := $(ROOT)
auth_client_SRCS = auth_client.c
auth_client_LIBS = $(AL_LIBS)

EXECUTABLES += nc_keygen
nc_keygen_DIR := $(ROOT)
nc_keygen_SRCS = nc_keygen.c
nc_keygen_LIBS = nc_keystore demoutil nc_udp monocypher

# Client and server on one loop over real loopback sockets: a round trip, a
# mismatched host key that must send nothing, and a peer that never answers
# (which is the regression test for a retransmit timer that was once
# scheduled and then quietly retired).
EXECUTABLES += test_auth_link
test_auth_link_DIR := $(ROOT)
test_auth_link_SRCS = test_auth_link.c
test_auth_link_LIBS = $(AL_LIBS)
define test_auth_link_TESTCMD
$(test_auth_link_RUN)
endef
TEST_TARGETS += test_auth_link

endif
