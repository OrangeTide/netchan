#!/bin/sh
# vendor.sh : copy a release of netchan into another project's tree
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
#
# Vendoring is the supported way to use netchan, so this does the copying:
# it fetches the source snapshot GitHub serves for a tag, takes the layers
# asked for and the ones they depend on, and records what it took so the
# next upgrade knows where it started.
#
#   curl -fsSL https://raw.githubusercontent.com/OrangeTide/netchan/main/tools/vendor.sh | sh
#
# That form takes the latest release. Piping a script from the network into a
# shell means trusting the server and the connection, so for a repeatable
# build name a tag instead: the URL then points at an immutable commit, and
# reading the script before running it is the better habit.
#
#   TAG=v0.5.0    # whichever release you want
#   curl -fsSLO "https://raw.githubusercontent.com/OrangeTide/netchan/$TAG/tools/vendor.sh"
#   less vendor.sh && sh vendor.sh --version "$TAG"

set -eu

# Overridable so a fork, a mirror, or an offline copy can be used in place of
# the canonical repository. The defaults are what anyone piping this from the
# network wants.
REPO=${NETCHAN_VENDOR_REPO:-OrangeTide/netchan}
BASE_URL=${NETCHAN_VENDOR_BASE_URL:-https://github.com}
API_URL=${NETCHAN_VENDOR_API_URL:-https://api.github.com}

VERSION=latest
DEST=third_party/netchan
LAYERS=
FORCE=0
LIST=0

usage() {
    cat <<'EOF'
Usage: vendor.sh [options]

  --version <tag>   release to take, e.g. v0.5.0, or "latest" (default)
  --layer <name>    what to copy; repeatable, defaults to core
  --dest <dir>      where to put it (default: third_party/netchan)
  --force           overwrite an existing destination
  --list            print what each layer copies, then exit
  --help            this

Layers, each of which pulls in what it needs:

  core     the protocol            src/
  udp      core + UDP addresses    transport/nc_udp.*
  ws       core + WebSocket codec  transport/nc_ws.*
  crypto   core + encryption       crypto/, third_party/
  auth     crypto + a login        auth/
  micro    the microchan variant   microchan/src/, microchan/transport/mc_udp.*
  all      everything above

Examples:

  vendor.sh --layer auth --layer udp --dest vendor/netchan
  vendor.sh --version v0.5.0 --layer core
EOF
}

# Each layer names its own files. Dependencies are resolved separately, so
# these lists stay honest about what the layer itself is.
layer_paths() {
    case "$1" in
    core)   echo "src" ;;
    udp)    echo "transport/nc_udp.c transport/nc_udp.h" ;;
    ws)     echo "transport/nc_ws.c transport/nc_ws.h" ;;
    crypto) echo "crypto third_party" ;;
    auth)   echo "auth" ;;
    micro)  echo "microchan/src microchan/transport/mc_udp.c microchan/transport/mc_udp.h" ;;
    *)      echo "" ;;
    esac
}

# A layer is useless without the ones below it, and someone asking for auth
# should not have to know that it needs crypto, which needs monocypher.
layer_deps() {
    case "$1" in
    core)   echo "" ;;
    udp)    echo "core" ;;
    ws)     echo "core" ;;
    crypto) echo "core" ;;
    auth)   echo "core crypto" ;;
    micro)  echo "" ;;
    *)      echo "" ;;
    esac
}

die() {
    echo "vendor.sh: $*" >&2
    exit 1
}

have() {
    command -v "$1" >/dev/null 2>&1
}

# curl and wget are both common and neither is guaranteed.
fetch() {
    url=$1
    out=$2
    if have curl; then
        curl -fsSL "$url" -o "$out"
    elif have wget; then
        wget -qO "$out" "$url"
    else
        die "need curl or wget"
    fi
}

fetch_stdout() {
    if have curl; then
        curl -fsSL "$1"
    elif have wget; then
        wget -qO- "$1"
    else
        die "need curl or wget"
    fi
}

while [ $# -gt 0 ]; do
    case "$1" in
    --version) [ $# -ge 2 ] || die "--version needs a value"; VERSION=$2; shift 2 ;;
    --dest)    [ $# -ge 2 ] || die "--dest needs a value";    DEST=$2;    shift 2 ;;
    --layer)   [ $# -ge 2 ] || die "--layer needs a value"
               LAYERS="$LAYERS $2"; shift 2 ;;
    --force)   FORCE=1; shift ;;
    --list)    LIST=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *)         usage >&2; die "unknown option: $1" ;;
    esac
done

if [ "$LIST" -eq 1 ]; then
    for l in core udp ws crypto auth micro; do
        printf '%-8s %s\n' "$l" "$(layer_paths "$l")"
    done
    exit 0
fi

[ -n "$LAYERS" ] || LAYERS=core

case " $LAYERS " in
*" all "*) LAYERS="core udp ws crypto auth micro" ;;
esac

# Expand dependencies and drop duplicates, so --layer auth --layer core does
# not copy src/ twice.
WANTED=
for l in $LAYERS; do
    [ -n "$(layer_paths "$l")" ] || die "unknown layer: $l (try --list)"
    for d in $(layer_deps "$l") "$l"; do
        case " $WANTED " in
        *" $d "*) ;;
        *) WANTED="$WANTED $d" ;;
        esac
    done
done

if [ "$VERSION" = latest ]; then
    echo "vendor.sh: asking for the latest release of $REPO"
    VERSION=$(fetch_stdout "$API_URL/repos/$REPO/releases/latest" \
              | sed -n 's/.*"tag_name" *: *"\([^"]*\)".*/\1/p' | head -n 1) || true
    [ -n "$VERSION" ] || die "could not determine the latest release; pass --version"
fi

case "$VERSION" in
v*) ;;
*)  VERSION="v$VERSION" ;;
esac

if [ -e "$DEST" ] && [ "$FORCE" -ne 1 ]; then
    die "$DEST already exists (use --force to overwrite, after checking what is in it)"
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/netchan-vendor.XXXXXX") ||
    die "cannot make a temporary directory"
trap 'rm -rf "$TMP"' EXIT INT TERM

URL="$BASE_URL/$REPO/archive/refs/tags/$VERSION.tar.gz"
echo "vendor.sh: fetching $URL"
fetch "$URL" "$TMP/src.tar.gz" ||
    die "cannot fetch $VERSION (does the tag exist?)"

have tar || die "need tar"
tar -xzf "$TMP/src.tar.gz" -C "$TMP" || die "cannot unpack the snapshot"

# GitHub names the directory <repo>-<tag without the v>, but that is its
# convention rather than a promise, so find it instead of assuming it.
SRC=
for d in "$TMP"/*/; do
    [ -d "$d" ] || continue
    [ -f "$d/src/netchan.h" ] || continue
    SRC=${d%/}
    break
done
[ -n "$SRC" ] || die "the snapshot does not look like netchan"

mkdir -p "$DEST"

# Overwriting with a different set of layers would otherwise leave the ones
# no longer asked for sitting there, stale and still compiling. Take out what
# the last run recorded before writing what this one wants.
if [ "$FORCE" -eq 1 ] && [ -f "$DEST/VENDORED.md" ]; then
    prev=$(sed -n 's/^| Layers | \(.*\) |$/\1/p' "$DEST/VENDORED.md")
    for l in $prev; do
        for p in $(layer_paths "$l"); do
            rm -rf "${DEST:?}/$p"
        done
    done
fi

copied=
for l in $WANTED; do
    for p in $(layer_paths "$l"); do
        [ -e "$SRC/$p" ] || die "$p is missing from $VERSION"
        mkdir -p "$DEST/$(dirname "$p")"
        rm -rf "${DEST:?}/$p"
        cp -R "$SRC/$p" "$DEST/$p"
        copied="$copied $p"
    done
done

# The licence travels with the code, always.
cp "$SRC/LICENSE" "$DEST/LICENSE"

# What a later upgrade needs to know, and what a reader needs to answer
# "where did this come from and is it current".
cat > "$DEST/VENDORED.md" <<EOF
# Vendored netchan

| | |
| --- | --- |
| Version | \`$VERSION\` |
| Source | <$URL> |
| Layers | ${WANTED# } |

Copied by \`tools/vendor.sh\` from the release snapshot. Do not edit these
files in place: local changes are lost on the next upgrade and make it
impossible to tell what version this is. Upgrade by re-running the script
with a newer \`--version\`, then read the upstream CHANGELOG for wire breaks.

Wire compatibility holds within a minor version and no further.
EOF

echo "vendor.sh: copied into $DEST"
for p in $copied; do
    echo "  $p"
done

echo
echo "Add the include paths your build needs, one per directory taken:"
printf ' '
for l in $WANTED; do
    case "$l" in
    core)   printf ' -I%s/src' "$DEST" ;;
    udp|ws) printf ' -I%s/transport' "$DEST" ;;
    crypto) printf ' -I%s/crypto -I%s/third_party' "$DEST" "$DEST" ;;
    auth)   printf ' -I%s/auth' "$DEST" ;;
    micro)  printf ' -I%s/microchan/src -I%s/microchan/transport' "$DEST" "$DEST" ;;
    esac
done
echo
echo
echo "Details are in $DEST/VENDORED.md."
