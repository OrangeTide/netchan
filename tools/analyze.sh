#!/bin/sh
# analyze.sh : build under gcc -fanalyzer and fail on a warning in our code
#
# The analyzer walks execution paths rather than matching patterns, so it
# finds the things a review misses: a descriptor leaked down one branch, a
# value used before its guard, a double free. It is slow and it is gcc-only,
# which is why this is its own target rather than part of the normal build.
#
# Vendored code is exempt, the same rule the linter follows. examples/iox
# carries four warnings upstream and fixing them in a copy would only make
# the next update harder.
#
#   sh tools/analyze.sh       analyze the whole tree
#   make analyze              the same thing through the build

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 1

cc=${CC:-gcc}
log=$(mktemp)
trap 'rm -f "$log"' EXIT INT TERM

# -std= is deliberately absent. Selecting strict ISO C hides the POSIX
# declarations this tree uses, and the analyzer then reports a cascade of
# implicit declarations that say nothing about the code.
"$cc" --version | head -n 1
make clean >/dev/null 2>&1
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
    CC="$cc" CFLAGS="-Wall -W -O1 -g -fanalyzer" >"$log" 2>&1
build_status=$?

ours=$(grep -E '^[^ ]+\.[ch]:[0-9]+:[0-9]+: (warning|error)' "$log" |
       grep -v '^third_party/' |
       grep -v '^examples/iox/' |
       sort -u)

if [ -n "$ours" ]; then
    echo "analyze: the analyzer reports:"
    printf '%s\n' "$ours" | sed 's/^/  /'
    exit 1
fi

if [ "$build_status" -ne 0 ]; then
    echo "analyze: the build itself failed"
    tail -n 30 "$log"
    exit 1
fi

echo "analyze: gcc -fanalyzer is clean over the project's own code"
