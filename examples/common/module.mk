# common -- shared by the example programs, of no interest to the library.
#
#   prompt.c    the one place anything here touches termios: a no-echo
#               password read and an incremental line read that leaves the
#               terminal in canonical mode.
#   sockutil.c  bind and connect boilerplate around nc_udp.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

ifneq ($(_TARGET_OS),Emscripten)
LIBRARIES += demoutil
demoutil_DIR := $(ROOT)
demoutil_SRCS = prompt.c sockutil.c
demoutil_LIBS = nc_udp
demoutil_EXPORTED_CPPFLAGS = -I$(demoutil_DIR)
endif
