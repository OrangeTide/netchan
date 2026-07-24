#!/bin/sh
# check-symbols.sh : fail if the manual calls a netchan function that is gone
# PUBLIC DOMAIN (CC0-1.0)
#
# Usage: check-symbols.sh built-manual.html src/netchan.h
#
# The API reference is generated from the header and cannot drift. The prose
# and the code samples are hand-written and can: a guide may go on calling a
# function that was renamed or removed. This finds every netchan_ call in the
# built page and fails if the header does not declare it, so a stale example
# breaks the build instead of shipping.

set -eu

PAGE=${1:?usage: check-symbols.sh page.html header.h}
HDR=${2:?usage: check-symbols.sh page.html header.h}

# Symbols the header declares or defines: functions, macros, enum values.
decls=$(grep -oE '\bnetchan_[A-Za-z0-9_]+' "$HDR" | sort -u)

# Symbols the page names as a call: netchan_xxx immediately before "(".
used=$(grep -oE '\bnetchan_[A-Za-z0-9_]+ *\(' "$PAGE" \
       | sed 's/ *($//; s/ *(//' | sort -u)

missing=""
for sym in $used; do
    if ! printf '%s\n' "$decls" | grep -qx "$sym"; then
        missing="$missing $sym"
    fi
done

if [ -n "$missing" ]; then
    echo "check-symbols: the manual calls symbols not in $HDR:" >&2
    for sym in $missing; do
        echo "  $sym" >&2
    done
    exit 1
fi

echo "check-symbols: every netchan_ call in the manual is declared in the header"
