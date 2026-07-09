#!/bin/sh
# nc_rtc_e2e_test.sh : prove the WebRTC gateway relays a datagram end to end,
#                      without a browser.
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
#
# Starts a UDP echo server (standing in for the game server) and the WebRTC
# gateway, then runs rtc_probe: a libpeer offerer that POSTs an SDP offer, opens
# a data channel, and sends a datagram. It must come back, having travelled
#   probe --data channel--> gateway --UDP--> echo --UDP--> gateway --dc--> probe
# Run ./build.sh first.

set -u
cd "$(dirname "$0")"
B="build"
GPORT=$((9700 + $$ % 200))
HPORT=$((8700 + $$ % 200))

for b in rtc_gateway rtc_probe udp_echo; do
    [ -x "$B/$b" ] || { echo "nc_rtc_e2e_test: FAIL: build $B/$b first (./build.sh)"; exit 1; }
done

"$B/udp_echo" "$GPORT" >/dev/null 2>&1 &
E=$!
"$B/rtc_gateway" "$HPORT" 127.0.0.1 "$GPORT" . >/tmp/nc_rtc_gw.$$ 2>&1 &
G=$!
trap 'kill "$E" "$G" 2>/dev/null; rm -f /tmp/nc_rtc_gw.$$' EXIT
sleep 1

"$B/rtc_probe" 127.0.0.1 "$HPORT" 2>/dev/null | grep -aE "probe:|PASS|FAIL"
RC=$?

if [ "$RC" -eq 0 ]; then
    echo "nc_rtc_e2e_test: PASS"
else
    echo "nc_rtc_e2e_test: FAIL (probe did not get its datagram back)"
fi
exit "$RC"
