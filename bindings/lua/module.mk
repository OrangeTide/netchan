# bindings/lua -- the netchan Lua C module.
#
# Builds libnetchan.so with a luaopen_netchan entry point, linked against the
# shim shared object. Lua finds it as the module "netchan" when
# package.cpath carries a "lib?.so" pattern, which is how the test and the
# example load it from the build tree.
#
# The Lua package name is set with LUA_PKG (default lua5.1); point it at
# whichever pkg-config name the host Lua installs.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

LUA_PKG ?= lua5.1

SHARED_LIBS += netchan
netchan_DIR := $(ROOT)
netchan_SRCS = netchan_lua.c
netchan_LIBS = netchan_c
netchan_PKGS = $(LUA_PKG)
# Resolve the sibling libnetchan_c.so from the module's own directory, so the
# module loads without LD_LIBRARY_PATH once both land in the same lib dir.
netchan_LDFLAGS = -Wl,-rpath,'$$ORIGIN'

# Build the module, then drive it from a plain Lua script. NCDIR hands the
# script the directory holding the shared objects; LD_LIBRARY_PATH lets the
# module resolve its libnetchan_c.so dependency at load time.
define netchan_TESTCMD
NCDIR=$(LIBDIR) LD_LIBRARY_PATH=$(LIBDIR) $(LUA) $(netchan_DIR)test.lua
endef
TEST_TARGETS += netchan

LUA ?= lua
