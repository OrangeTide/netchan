#!/bin/sh
# smoke_test.sh : run the beacon demo for real, two processes and a socket
#
# The unit test drives nc_beacon with buffers. Nothing drove it with a socket
# under it, and the parts a buffer cannot reach are exactly the parts that go
# wrong: whether the packet a server sends is the packet a browser recognises,
# whether a probe gets answered at all, and whether the local-address check
# lets loopback through.
#
# Everything here runs over 127.0.0.1. Broadcast is the first thing a
# container or a wireless access point drops, so a test that needed it would
# fail for reasons that have nothing to do with this code.
#
# The two sides use different ports rather than sharing one, because two
# sockets bound to the same port with SO_REUSEPORT split incoming datagrams
# between them and the test would pass or fail by coin toss.
#
# Usage: smoke_test.sh "<command that runs beacon_demo>"
#
# The argument is a command rather than a path so a wrapper survives, which is
# what makes 'make TESTWRAP="valgrind" run-test-beacon_demo' work.

set -eu

RUN=${1:?usage: smoke_test.sh "<command that runs beacon_demo>"}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/netchan-beacon.XXXXXX")
SERVE_PORT=19901
BROWSE_PORT=19902
NAME="Ashen Coast"

cleanup() {
    [ -n "${SERVER_PID:-}" ] && kill "$SERVER_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

fail() {
    echo "FAIL: $1" >&2
    echo "--- server output ---" >&2
    cat "$WORK/server.log" >&2 2>/dev/null || true
    echo "--- browser output ---" >&2
    cat "$WORK/browse.log" >&2 2>/dev/null || true
    exit 1
}

# The server announces straight at the browser's port, so the announce path is
# exercised without a broadcast packet.
$RUN serve --port "$SERVE_PORT" --name "$NAME" \
     --to "127.0.0.1:$BROWSE_PORT" --every 500 > "$WORK/server.log" 2>&1 &
SERVER_PID=$!

sleep 1
kill -0 "$SERVER_PID" 2>/dev/null || fail "server exited at startup"

# The browser probes the server's port and listens on its own, so both the
# probe reply and the timed announcement reach it.
$RUN browse --port "$BROWSE_PORT" --to "127.0.0.1:$SERVE_PORT" \
     --seconds 3 > "$WORK/browse.log" 2>&1 || fail "browser exited non-zero"

grep -q "found" "$WORK/browse.log" || fail "browser found no server"
grep -q "$NAME" "$WORK/browse.log" || fail "the server's name did not survive"

# The probe has to have been answered, not merely the announcement received.
# Silence from the server's log is the check: a refused probe says so.
if grep -q "ignored" "$WORK/server.log"; then
    fail "the loopback probe was treated as non-local"
fi

echo "ok: a server on loopback is announced, probed, and found"
