#!/bin/sh
# smoke_test.sh : run the chat example for real, two processes and a signal
#
# The unit tests drive the library. Nothing drove the example, so a crash in
# its event loop could sit in the tree while every test passed, and one did:
# closing a peer from inside the poll loop left the loop polling the slot it
# had just cleared. That needed a client to leave, which needed a DISCONNECT
# to arrive, which is exactly what netchan_disconnect made ordinary.
#
# The test starts the server, connects a client, interrupts the client, and
# requires the server to survive that and to report the departure. It takes
# about eight seconds against a thirty second idle timeout, so a server that
# reports the disconnect can only have learned it from a frame on the wire.
#
# Usage: smoke_test.sh "<command that runs netchan_example>"
#
# The argument is a command rather than a path so a wrapper survives, which is
# what makes 'make TESTWRAP="valgrind" run-test-netchan_example' work.

set -eu

RUN=${1:?usage: smoke_test.sh "<command that runs netchan_example>"}

# With a template because BSD mktemp, which is what macOS has, requires one.
WORK=$(mktemp -d "${TMPDIR:-/tmp}/netchan-smoke.XXXXXX")
SRV_PID=
CLI_PID=

cleanup() {
    [ -z "$CLI_PID" ] || kill "$CLI_PID" 2>/dev/null || :
    [ -z "$SRV_PID" ] || kill "$SRV_PID" 2>/dev/null || :
    exec 3>&- 2>/dev/null || :
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

fail() {
    echo "FAIL: $1" >&2
    echo "--- server output ---" >&2
    cat "$WORK/srv.log" >&2 2>/dev/null || :
    echo "--- client output ---" >&2
    cat "$WORK/cli.log" >&2 2>/dev/null || :
    exit 1
}

# Both programs print to a redirected stdout, which libc buffers, so their
# logs are only complete once they have exited. Everything is checked at the
# end for that reason, and the waits below are plain sleeps with room to
# spare rather than polls of a file that has nothing in it yet.
say() {
    echo "  $1"
}

# $RUN is deliberately unquoted: it may carry a wrapper and its arguments.
say "starting the server"
( exec $RUN server ) > "$WORK/srv.log" 2>&1 &
SRV_PID=$!
sleep 1

# Hold the client's stdin open. It polls stdin for chat lines once connected,
# and at EOF that is always ready, which would spin a core for the whole run.
# Opened read-write because opening a fifo for writing alone blocks until a
# reader arrives, and the reader here is a process this script has not started.
mkfifo "$WORK/keyboard"
exec 3<> "$WORK/keyboard"

say "connecting a client"
( exec $RUN client tester ) < "$WORK/keyboard" > "$WORK/cli.log" 2>&1 &
CLI_PID=$!
sleep 3      # connect, name exchange; the client retries, so this is slack

say "interrupting the client"
kill -INT "$CLI_PID"
CLI_STATUS=0
wait "$CLI_PID" || CLI_STATUS=$?
CLI_PID=
[ "$CLI_STATUS" -eq 0 ] || fail "the client exited with status $CLI_STATUS"

sleep 2      # time for the server to read the DISCONNECT and act on it

# The regression this exists for: the server used to die here, polling a
# connection it had freed and a slot it had zeroed.
kill -0 "$SRV_PID" 2>/dev/null || fail "the server died when the client left"

say "stopping the server"
kill -INT "$SRV_PID"
SRV_STATUS=0
wait "$SRV_PID" || SRV_STATUS=$?
SRV_PID=
[ "$SRV_STATUS" -eq 0 ] || fail "the server exited with status $SRV_STATUS"

for pattern in \
    'server listening' \
    'peer 0 connected' \
    'peer 0 is "tester"' \
    'peer 0 ("tester") disconnected' \
    'server: shutdown'
do
    grep -qF "$pattern" "$WORK/srv.log" || fail "no \"$pattern\" from the server"
done
grep -qF 'connected!' "$WORK/cli.log" || fail "the client never connected"

echo "ok: the chat example connects, disconnects, and survives both"
