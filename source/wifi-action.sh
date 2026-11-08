#!/bin/sh
# Called by wpa_cli when Wi-Fi connects or disconnects: get / drop an IP address.
IF=$1
PID=/run/udhcpc-$IF.pid
stop_dhcp() { [ -f "$PID" ] && kill "$(cat "$PID")" 2>/dev/null; rm -f "$PID"; }
case "$2" in
    CONNECTED)
        stop_dhcp
        udhcpc -i "$IF" -s /usr/share/udhcpc/default.script -t 6 -T 2 -A 5 -b -p "$PID" >/dev/null 2>&1
        ;;
    DISCONNECTED)
        stop_dhcp
        ip route del default dev "$IF" 2>/dev/null
        ip addr flush dev "$IF" 2>/dev/null
        ;;
esac
exit 0
