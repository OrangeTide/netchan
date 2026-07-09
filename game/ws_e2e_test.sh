#!/bin/sh
# ws_e2e_test.sh : end-to-end proof that a WebSocket client and a native UDP
#                  client share one unmodified game server.
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
#
# Launches the game server and the WebSocket gateway, then runs a WS client
# (through the gateway) and a native UDP client (straight to the server) at the
# same time. Both must join and receive snapshots. Pass the directory holding
# the built binaries as $1, or set BIN; it defaults to the modular-make output.

set -u

BIN="${1:-${BIN:-_out/x86_64-linux-gnu/bin}}"
GPORT=$((9000 + $$ % 500))
WPORT=$((8000 + $$ % 500))

fail() { echo "ws_e2e_test: FAIL: $*" >&2; exit 1; }

for b in game_server ws_gateway ws_client game_client; do
    [ -x "$BIN/$b" ] || fail "missing binary $BIN/$b (build first)"
done

"$BIN/game_server" "$GPORT" >/tmp/e2e_srv.$$ 2>&1 &
SRV=$!
"$BIN/ws_gateway" "$WPORT" 127.0.0.1 "$GPORT" web >/tmp/e2e_gw.$$ 2>&1 &
GW=$!

cleanup() { kill "$SRV" "$GW" 2>/dev/null; rm -f /tmp/e2e_srv.$$ /tmp/e2e_gw.$$; }
trap cleanup EXIT

# give the listeners a moment to bind
i=0
while [ $i -lt 50 ]; do
    if grep -q "ws gateway on" /tmp/e2e_gw.$$ 2>/dev/null; then break; fi
    i=$((i + 1)); sleep 0.05 2>/dev/null || sleep 1
done

# a browser (WebSocket) player and a terminal (UDP) player, concurrently
"$BIN/ws_client"   127.0.0.1 "$WPORT" 2500 2 >/tmp/e2e_ws.$$ 2>&1 &
WS=$!
"$BIN/game_client" 127.0.0.1 "$GPORT" 2500 6 >/tmp/e2e_udp.$$ 2>&1 &
UDP=$!
wait "$WS"; WS_RC=$?
wait "$UDP"; UDP_RC=$?

echo "--- WebSocket client ---"; cat /tmp/e2e_ws.$$
echo "--- native UDP client ---"; cat /tmp/e2e_udp.$$
rm -f /tmp/e2e_ws.$$ /tmp/e2e_udp.$$

[ "$WS_RC" -eq 0 ]  || fail "WebSocket client did not join / get snapshots"
[ "$UDP_RC" -eq 0 ] || fail "native UDP client did not join / get snapshots"
grep -q "snapshots=[1-9]" /tmp/e2e_srv.$$ 2>/dev/null    # (server log optional)

echo "ws_e2e_test: PASS (WebSocket + UDP clients shared one server)"
