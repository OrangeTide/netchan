#!/bin/sh
# Manage the IPX relay used for local testing. Only ever touches the relay
# instance bound to our test port (matched by the exact -p argument), so it
# never disturbs other relays the user may be running.
#
# Usage: scripts/relay.sh [start|stop|restart|status]
#   start    launch if not already up
#   restart  bounce it (clears stale client registrations)
#   stop     terminate our instance
#   status   show our instance, if any
RELAY_DIR="${IPXRELAY_DIR:-$HOME/Source/ipxrelay}"
PORT="${RELAY_PORT:-19900}"
PAT="ipxrelay -F -p $PORT"

up() {
    "$RELAY_DIR/ipxrelay" -F -p "$PORT" -a 127.0.0.1 >/tmp/ipxrelay.log 2>&1 &
}

case "${1:-status}" in
start)
    pgrep -f "$PAT" >/dev/null 2>&1 || up
    echo "relay: $(pgrep -f "$PAT" | tr '\n' ' ')"
    ;;
stop)
    pkill -f "$PAT" 2>/dev/null
    echo "relay stopped"
    ;;
restart)
    pkill -f "$PAT" 2>/dev/null
    sleep 1
    up
    sleep 1
    echo "relay: $(pgrep -f "$PAT" | tr '\n' ' ')"
    ;;
status)
    pgrep -fa "$PAT" | grep -v 'pgrep' || echo "relay not running"
    ;;
*)
    echo "usage: $0 [start|stop|restart|status]"
    exit 2
    ;;
esac
