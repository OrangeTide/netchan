#!/bin/sh
# microser-gen.sh : compile a microser .idl into C structs and codecs
#
# Usage: microser-gen.sh input.idl output_basename
#
# Writes output_basename.h and output_basename.c. The basename may carry a
# directory, which the generated #include deliberately does not repeat: the
# .c includes its header by bare name, so a build that generates into its
# own directory compiles with that directory on the include path.
#
# The IDL:
#
#   enum Kind          # names for a discriminant, each a uint8
#       Ping = 1
#   end
#
#   message Hello      # a struct plus encode and decode functions
#       uint32 id = 1  # <type> <name> = <field number, 1 to 31>
#       string who = 2
#       case Kind kind = 3    # a tagged union, on a discriminant field
#           Ping:
#               uint16 seq = 4
#           end
#       end
#   end
#
# The compiler proper is microser-gen.awk, invoked with -f so its source is a
# plain file rather than a shell-quoted string. This wrapper only checks the
# arguments and finds the awk beside itself.

set -e

INPUT="$1"
BASE="$2"

if [ -z "$INPUT" ] || [ -z "$BASE" ]; then
    echo "usage: $0 input.idl output_basename" >&2
    exit 1
fi

dir=$(dirname "$0")
exec awk -v base="$BASE" -f "$dir/microser-gen.awk" "$INPUT"
