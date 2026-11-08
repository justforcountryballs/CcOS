#!/bin/sh
# Called by udhcpc (the DHCP client) to apply the network settings it got.
# Cable (eth*) is preferred over Wi-Fi when both are connected, like Windows.
case "$interface" in eth*) METRIC=100 ;; *) METRIC=600 ;; esac
case "$1" in
    deconfig)
        ip addr flush dev "$interface"
        ip link set "$interface" up
        ;;
    bound|renew)
        ip addr flush dev "$interface"
        ip addr add "$ip/${mask:-24}" dev "$interface"
        if [ -n "$router" ]; then
            ip route del default dev "$interface" 2>/dev/null
            for r in $router; do ip route add default via "$r" dev "$interface" metric $METRIC; break; done
        fi
        if [ -n "$dns" ]; then
            : > /etc/resolv.conf
            for d in $dns; do echo "nameserver $d" >> /etc/resolv.conf; done
        fi
        ;;
esac
exit 0
