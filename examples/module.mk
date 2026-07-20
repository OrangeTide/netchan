# examples -- programs that show the library in use.
#
# Nothing here is part of the library and a vendoring project is expected to
# leave the whole directory behind. Build without it:
#
#   make NETCHAN_EXAMPLES=0
#
#   iox/         a vendored poll() event loop. The library does not use one,
#                and does not care which one you use; the echo and auth
#                examples need something, and this is what they picked.
#   common/      terminal and socket odds and ends the programs share.
#   chat/        two peers, plain UDP, no crypto. The smallest thing that
#                exercises netchan end to end.
#   ws_gateway/  relays browser WebSocket clients onto an unmodified UDP
#                server as ordinary peers.
#   echo/        an encrypted session: netchan over nc_crypto, no login.
#   auth/        the full stack, with host keys, known_hosts, and an
#                ssh-shaped login.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

SUBDIRS = iox common chat ws_gateway echo auth
