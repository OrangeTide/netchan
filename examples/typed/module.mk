# typed -- structured messages on a channel, via microser and a dispatch table.
#
# chat.idl is compiled to chat.c/.h at build time, the same way tests/ builds
# its proto. The program is socketless: it wires two connections with an
# in-process pump so the message flow is all that shows. It doubles as a test.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

ifneq ($(_TARGET_OS),Emscripten)

EXECUTABLES += typed_example
typed_example_DIR := $(ROOT)
typed_example_SRCS = typed_example.c
typed_example_GENERATED_SRCS = chat.c
typed_example_GENERATED_HDRS = chat.h
typed_example_CPPFLAGS = $(NETCHAN_IDL_INC)
typed_example_LIBS = netchan_core

# Two recipes, not a grouped `&:` target, so macOS's GNU Make 3.81 rebuilds a
# consumer when only the header changes. See tests/module.mk for the same.
$(BUILDDIR)/$(typed_example_DIR)chat.c : \
		$(typed_example_DIR)chat.idl $(MICROSER_GEN_DEPS)
	$(MICROSER_GEN) $< $(BUILDDIR)/$(typed_example_DIR)chat
$(BUILDDIR)/$(typed_example_DIR)chat.h : \
		$(BUILDDIR)/$(typed_example_DIR)chat.c ; @touch -c $@

define typed_example_TESTCMD
$(typed_example_RUN)
endef
TEST_TARGETS += typed_example

endif
