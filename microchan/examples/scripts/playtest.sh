#!/bin/sh
# Networked thor playtest harness (development tool).
#
# Builds thor.exe, ensures an ipxrelay, launches a host and a client DOSBox
# through it, and (with the 'dump' arg) each writes its 40x25 screen to
# SVIEW.TXT / CVIEW.TXT once a second so a headless run can be inspected.
# Only manages the two DOSBox instances it starts.
#
# Run from anywhere; paths are derived from this script's location.
#
# Usage: examples/scripts/playtest.sh [seconds]
set -u
# The Open Watcom build lives at the microchan root; its .exe land in _dos/,
# and that is the directory DOSBox mounts as C:.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DIR="$ROOT/_dos"
TMO="${1:-16}"
RELAY_PORT="${RELAY_PORT:-19900}"
: "${WATCOM:?set WATCOM to your Open Watcom install}"
export PATH="$WATCOM/binl:$PATH"
cd "$ROOT" || exit 1

if wmake 2>&1 | grep -iE 'error|warning'; then
    echo "BUILD FAILED"; exit 1
fi
sh "$ROOT/examples/scripts/relay.sh" restart >/dev/null
cd "$DIR" || exit 1
rm -f SVIEW.TXT CVIEW.TXT

mkconf() {
    cat > "$1" <<EOF
[sdl]
output=surface
[cpu]
cycles=20000
[ipx]
ipx=true
[autoexec]
mount c $DIR
c:
ipxnet connect 127.0.0.1 $RELAY_PORT
$2
exit
EOF
}

mkconf /tmp/thor_s.conf "thor.exe s dump"
mkconf /tmp/thor_c.conf "thor.exe dump"

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    timeout -k2 "$TMO" dosbox -noconsole -conf /tmp/thor_s.conf >/dev/null 2>&1 &
PS=$!
sleep 3
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    timeout -k2 "$TMO" dosbox -noconsole -conf /tmp/thor_c.conf >/dev/null 2>&1 &
PC=$!
wait "$PC"; wait "$PS"

echo "================ SERVER (SVIEW.TXT) ================"
cat SVIEW.TXT 2>/dev/null || echo "(none)"
echo "================ CLIENT (CVIEW.TXT) ================"
cat CVIEW.TXT 2>/dev/null || echo "(none)"
