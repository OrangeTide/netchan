# bindings -- optional language bindings for netchan.
#
# A flat C shim (netchan_bind.c) wraps the libraries in opaque handles and
# field accessors, so a binding never depends on a netchan struct layout. It
# is built as one shared object, libnetchan_c.so, that pulls in the core it
# references. Both bindings load that object:
#
#   make NETCHAN_LUA=1       the Lua C module (needs Lua headers)
#   make NETCHAN_PASCAL=1    the Free Pascal unit and demo (needs fpc)
#
# Both are off by default. The parent module.mk only descends here when one
# is on, and turns on -fPIC for the tree then. Neither targets emscripten.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

ifneq ($(_TARGET_OS),Emscripten)

# The shim as a shared object. A shared library links only its direct _LIBS,
# so every archive it draws on is named here, monocypher included, rather
# than left to transitive resolution. The linker group lets their mutual
# references resolve regardless of the order they are listed, so the result
# is self-contained with no undefined symbols.
SHARED_LIBS += netchan_c
netchan_c_DIR := $(ROOT)
netchan_c_SRCS = netchan_bind.c
netchan_c_LIBS = netchan_core nc_udp nc_crypto nc_auth nc_keystore nc_beacon \
                 monocypher
# A soname so a consumer records "libnetchan_c.so" as its dependency rather
# than the relative path the linker was handed, which would only resolve from
# the repository root.
netchan_c_LDFLAGS = -Wl,-soname,libnetchan_c.so -Wl,--start-group
netchan_c_LDLIBS = -Wl,--end-group
netchan_c_EXPORTED_CPPFLAGS = -I$(netchan_c_DIR)

ifeq ($(NETCHAN_PASCAL),1)
SUBDIRS += pascal
endif

ifeq ($(NETCHAN_LUA),1)
SUBDIRS += lua
endif

endif
