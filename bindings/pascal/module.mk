# bindings/pascal -- the netchan Free Pascal unit and its demo.
#
# The unit netchan.pas binds the shim's exported functions from
# libnetchan_c.so. fpc links Pascal programs itself, with its own runtime, so
# the demo is not a modular-make target: it is compiled by the rule below and
# run as the test of the shared object it loads.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

pascal_DIR := $(ROOT)
PASCAL_OUT := $(BUILDDIR)/$(pascal_DIR)

# nc_demo is the example and the smoke test in one. -Fu finds the unit, -Fl
# finds libnetchan_c.so to link against, and LD_LIBRARY_PATH finds it at run
# time. The test is attached to the netchan_c target so the shared object is
# built first.
define netchan_c_TESTCMD
$(MKDIR_P) $(PASCAL_OUT)
$(FPC) -Fu$(pascal_DIR) -Fl$(LIBDIR) -FE$(PASCAL_OUT) $(pascal_DIR)nc_demo.pas
LD_LIBRARY_PATH=$(LIBDIR) $(PASCAL_OUT)nc_demo
endef
TEST_TARGETS += netchan_c

FPC ?= fpc
