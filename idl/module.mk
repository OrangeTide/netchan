# idl -- microser: message definitions compiled to C structs and codecs.
#
# netchan channels carry opaque bytes. microser is how you decide what those
# bytes mean: write a .idl, and microser-gen.sh emits a struct plus an encode
# and a decode function for each message in it.
#
# There is no library here. microser.h is header-only, and the compiler is a
# shell script that runs at build time, so this layer exports an include path
# and the path to the generator rather than an archive to link.
#
# A dependent adds the include path and lists its generated source:
#
#   myapp_CPPFLAGS = $(NETCHAN_IDL_INC)
#   myapp_GENERATED_SRCS = proto.c
#   $(BUILDDIR)/$(myapp_DIR)proto.c $(BUILDDIR)/$(myapp_DIR)proto.h &: \
#   		$(myapp_DIR)proto.idl $(MICROSER_GEN_DEPS)
#   	$(MICROSER_GEN) $< $(BUILDDIR)/$(myapp_DIR)proto
#
# Depends on nothing but the C library, and needs a POSIX awk at build time.

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

# microser.h is included by generated code, which is compiled from BUILDDIR,
# so both the generated file and anything using it need this on the path.
NETCHAN_IDL_INC := -I$(ROOT)

# The IDL compiler. MICROSER_GEN is the command; MICROSER_GEN_DEPS is what a
# codegen rule lists as a prerequisite so it reruns when the compiler changes,
# not only when the .idl does. The .sh is a thin wrapper and the .awk is the
# compiler proper, so both belong in the dependency.
MICROSER_GEN := $(ROOT)microser-gen.sh
MICROSER_GEN_DEPS := $(ROOT)microser-gen.sh $(ROOT)microser-gen.awk
