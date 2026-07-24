#!/bin/sh
# build.sh : assemble the netchan manual into a single self-contained page
# PUBLIC DOMAIN (CC0-1.0)
#
# Usage: docs/build.sh [output-dir]     (default: docs/_site)
#
# Renders the diagrams to SVG, generates the API reference from the header,
# stamps the version, folds the CSS in, and writes one index.html with nothing
# fetched at view time. Needs mscgen, graphviz (dot), and a POSIX awk/sed.
# Runs the same locally and in CI.

set -eu

# So a CDPATH in the environment cannot redirect the cd's below.
unset CDPATH

here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/.." && pwd)
out=${1:-$here/_site}

command -v mscgen >/dev/null || { echo "build.sh: mscgen not found" >&2; exit 1; }
command -v dot >/dev/null || { echo "build.sh: graphviz dot not found" >&2; exit 1; }

mkdir -p "$out"
work=$(mktemp -d "${TMPDIR:-/tmp}/netchan-docs.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

version=$(sed -n 's/^#define NETCHAN_VERSION_STRING "\(.*\)"$/\1/p' \
          "$root/src/netchan.h")
[ -n "$version" ] || { echo "build.sh: no version in netchan.h" >&2; exit 1; }

# Make an SVG theme-aware and responsive: drop the fixed pixel size so the
# viewBox drives scaling, and swap the baked-in black and white for the page's
# text colour and a background variable, so one file serves light and dark.
# Separate px and pt expressions rather than a \(px\|pt\) alternation, which
# is a GNU sed extension BSD sed (macOS) does not accept.
postprocess_svg() {
    sed -e 's/ width="[0-9.]*px"//g' \
        -e 's/ height="[0-9.]*px"//g' \
        -e 's/ width="[0-9.]*pt"//g' \
        -e 's/ height="[0-9.]*pt"//g' \
        -e 's/"#000000"/"currentColor"/g' \
        -e 's/"#ffffff"/"var(--diagram-bg)"/g' \
        -e 's/="black"/="currentColor"/g' \
        -e 's/="white"/="var(--diagram-bg)"/g' \
        -e 's/fill:#000000/fill:currentColor/g' \
        -e 's/stroke:#000000/stroke:currentColor/g'
}

# Render each diagram to a post-processed SVG named for its stem.
for src in "$here"/diagrams/*.msc; do
    [ -e "$src" ] || continue
    name=$(basename "$src" .msc)
    mscgen -T svg -o "$work/$name.raw.svg" "$src"
    postprocess_svg < "$work/$name.raw.svg" > "$work/dia_$name.svg"
done
for src in "$here"/diagrams/*.dot; do
    [ -e "$src" ] || continue
    name=$(basename "$src" .dot)
    dot -Tsvg "$src" > "$work/$name.raw.svg"
    postprocess_svg < "$work/$name.raw.svg" > "$work/dia_$name.svg"
done

# Generate the API reference from the header.
sh "$here/gen-api.sh" "$root/src/netchan.h" > "$work/api.html"

# Assemble. awk reads the template and expands each placeholder, pulling the
# CSS, the API block, and each @DIAGRAM:name@ from files. Kept out of sed so a
# diagram's markup can hold any character without escaping.
awk -v version="$version" \
    -v css="$here/css/manual.css" \
    -v api="$work/api.html" \
    -v diadir="$work" '
function slurp(path,    line, acc) {
    acc = ""
    while ((getline line < path) > 0)
        acc = acc line "\n"
    close(path)
    return acc
}
{
    line = $0
    gsub(/@VERSION@/, version, line)
    if (line ~ /@STYLE@/)      { printf "%s", slurp(css); next }
    if (line ~ /@API@/)        { printf "%s", slurp(api); next }
    if (line ~ /@DIAGRAM:[a-z_]+@/) {
        name = line
        sub(/.*@DIAGRAM:/, "", name)
        sub(/@.*/, "", name)
        printf "%s", slurp(diadir "/dia_" name ".svg")
        next
    }
    print line
}
' "$here/manual.html.in" > "$out/index.html"

# .nojekyll stops GitHub Pages running the output through Jekyll, which would
# mangle any file whose name starts with an underscore.
: > "$out/.nojekyll"

# Guard the hand-written prose against calling a function the header dropped.
sh "$here/check-symbols.sh" "$out/index.html" "$root/src/netchan.h"

echo "build.sh: wrote $out/index.html (netchan $version)"
