#!/bin/sh
# gen-api.sh : turn a netchan header into the manual's API reference
#
# Usage: gen-api.sh path/to/netchan.h > api.html
#
# Emits an <h2> for each section banner in the header and an entry for each
# exported function, with the comment block above it as its documentation. The
# header is the single source of truth, so the reference cannot drift from the
# code. check-symbols.sh guards the other direction, the prose naming a symbol
# the header no longer declares.
#
# A section banner with no functions under it is dropped, so the reference
# lists only the parts of the header that declare something.
#
# The generator proper is gen-api.awk, invoked with -f so its source is a plain
# file rather than a shell-quoted string. This wrapper only checks its argument
# and finds the awk beside itself.

set -eu

HDR=${1:?usage: gen-api.sh path/to/netchan.h}

dir=$(dirname "$0")
exec awk -f "$dir/gen-api.awk" "$HDR"
