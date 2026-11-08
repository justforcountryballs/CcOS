#!/bin/sh
# ccos-bt - Bluetooth for the ccos desktop (Settings > Bluetooth), using BlueZ
#   ccos-bt status        one line per thing, written to /run/ccos/bt.txt:
#                           A <on|off|none>           the adapter
#                           S <1|0>                   scanning right now
#                           D MAC|name|icon|paired|connected
#   ccos-bt power on|off
#   ccos-bt scan          look for devices for 20 seconds
#   ccos-bt pair MAC      pair, trust and connect (headphones, mice, speakers)
#   ccos-bt connect MAC | disconnect MAC | remove MAC
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
out=/run/ccos/bt.txt
mkdir -p /run/ccos
bt() { bluetoothctl "$@" 2>/dev/null; }

status() {
    tmp=$out.$$
    if ! ls /sys/class/bluetooth/hci* >/dev/null 2>&1; then
        echo "A none" > $tmp; mv $tmp $out; return
    fi
    info=$(bt show)
    case "$info" in *"Powered: yes"*) echo "A on" ;; *"Powered:"*) echo "A off" ;; *) echo "A none" ;; esac > $tmp
    case "$info" in *"Discovering: yes"*) echo "S 1" ;; *) echo "S 0" ;; esac >> $tmp
    bt devices | while read -r _ mac name; do
        [ -n "$mac" ] || continue
        d=$(bt info "$mac")
        icon=$(printf '%s\n' "$d" | sed -n 's/^[[:space:]]*Icon: //p' | head -1)
        nm=$(printf '%s\n' "$d" | sed -n 's/^[[:space:]]*Alias: //p' | head -1)
        p=0; c=0
        case "$d" in *"Paired: yes"*) p=1 ;; esac
        case "$d" in *"Connected: yes"*) c=1 ;; esac
        # nameless devices (just an address) are noise: skip them unless paired
        [ "$p" = 0 ] && [ "$nm" = "$(echo "$mac" | tr ':' '-')" ] && continue
        echo "D $mac|${nm:-$name}|${icon:-unknown}|$p|$c" | tr -d '\r'
    done >> $tmp
    mv $tmp $out
}

case "$1" in
    status) status ;;
    power)  bt power "$2" >/dev/null; status ;;
    scan)   bt power on >/dev/null; bt --timeout 20 scan on >/dev/null; status ;;
    pair)
        bt --agent NoInputNoOutput --timeout 25 pair "$2" >/tmp/bt-pair.log
        bt trust "$2" >/dev/null
        bt --timeout 10 connect "$2" >/dev/null
        status ;;
    connect)    bt --timeout 10 connect "$2" >/dev/null; status ;;
    disconnect) bt disconnect "$2" >/dev/null; status ;;
    remove)     bt remove "$2" >/dev/null; status ;;
    *) echo "usage: ccos-bt status|power on|off|scan|pair MAC|connect MAC|disconnect MAC|remove MAC" >&2; exit 1 ;;
esac
