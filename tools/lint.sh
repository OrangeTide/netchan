#!/bin/sh
# lint.sh : check the tree against docs/coding-style.md
#
# Mechanical rules only. It cannot judge whether a name is good or a comment
# earns its place, but it does catch the conventions that silently drift:
# tabs, trailing whitespace, per-file licence lines, bare -1 returns,
# declarations below the first statement, and so on.
#
# Vendored code is exempt. third_party/ and examples/iox/ keep their upstream
# style so that updating them stays a clean copy.
#
#   sh tools/lint.sh          check the whole tree
#   make lint                 the same thing through the build

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 1

rc=0
awk_prog=tools/lint-decls.awk

# Report a failure and remember it for the exit status.
fail() {
    printf '%s\n' "$1"
    rc=1
}

# Every C source and header the project owns, one per line.
owned_c() {
    git ls-files '*.c' '*.h' |
        grep -v '^third_party/' |
        grep -v '^examples/iox/'
}

# Shell and awk the project owns.
owned_scripts() {
    git ls-files '*.sh' '*.awk' | grep -v '^third_party/'
}

check_pattern() {
    what=$1
    pattern=$2
    hits=$(owned_c | xargs grep -nP "$pattern" 2>/dev/null)
    if [ -n "$hits" ]; then
        fail "lint: $what"
        printf '%s\n' "$hits" | sed 's/^/  /'
    fi
}

check_pattern "hard tabs (indent with 4 spaces)" '^\t'
check_pattern "trailing whitespace" ' +$'
check_pattern "non-ASCII character" '[^\x00-\x7F]'
check_pattern "#pragma once (use an include guard)" 'pragma once'
check_pattern "per-file licence line (the root LICENSE is the one place)" 'PUBLIC DOMAIN'

# The same rule for shell and awk. Anchored to a comment line, so this file
# does not report the pattern it searches for.
script_licence=$(owned_scripts |
                 xargs grep -nE '^#.*PUBLIC DOMAIN \(CC0' 2>/dev/null)
if [ -n "$script_licence" ]; then
    fail "lint: per-file licence line in a script"
    printf '%s\n' "$script_licence" | sed 's/^/  /'
fi
check_pattern "extern on a function declaration" '^extern .*\('
check_pattern "bare -1 return (name the module's failure value)" 'return -1;'

# A file has to end in exactly one newline.
for f in $(owned_c); do
    if [ -n "$(tail -c 1 "$f")" ]; then
        fail "lint: no final newline: $f"
    fi
done

# 78 columns is the target and 100 the ceiling.
long=$(owned_c | xargs awk 'length > 100 { printf "%s:%d: %d columns\n", FILENAME, FNR, length }')
if [ -n "$long" ]; then
    fail "lint: line over 100 columns"
    printf '%s\n' "$long" | sed 's/^/  /'
fi

# A definition puts its return type on the line above the name.
sameline=$(owned_c | xargs grep -nE '^(static +)?[a-z_0-9]+ +\**[a-z_0-9]+\([^;]*\)[[:space:]]*\{' 2>/dev/null)
if [ -n "$sameline" ]; then
    fail "lint: function definition with its body on the signature line"
    printf '%s\n' "$sameline" | sed 's/^/  /'
fi

# Every file opens with "<filename> : what it is".
for f in $(owned_c); do
    base=${f##*/}
    if ! head -n 1 "$f" | grep -q "^/\* $base : \|^/\*$"; then
        fail "lint: missing or malformed tag line: $f"
    fi
done

# Declarations belong at the top of their block.
for f in $(owned_c | grep '\.c$'); do
    awk -f "$awk_prog" "$f" || rc=1
done

# The scripts have their own small rules.
for f in $(owned_scripts); do
    case $f in
    *.sh)
        if [ -n "$(command -v shellcheck)" ]; then
            shellcheck -S warning "$f" || rc=1
        fi
        ;;
    *.awk)
        if ! awk -f "$f" /dev/null >/dev/null 2>&1; then
            # A generator that refuses to run without arguments is fine; a
            # syntax error is not. Tell them apart by asking for a parse.
            if awk -f "$f" /dev/null 2>&1 | grep -qi 'syntax error'; then
                fail "lint: awk syntax error: $f"
            fi
        fi
        ;;
    esac
done

if [ "$rc" -eq 0 ]; then
    echo "lint: the tree matches docs/coding-style.md"
fi
exit "$rc"
