#!/bin/sh
# ccos-network: start cable (Ethernet) and Wi-Fi networking. Run at boot.
ip link set lo up 2>/dev/null
CONF=/data/ccos.conf
NET=1; WIFI=1
[ -f "$CONF" ] && grep -q '^net=0' "$CONF" && NET=0
[ -f "$CONF" ] && grep -q '^wifi=0' "$CONF" && WIFI=0
mkdir -p /run/wpa_supplicant

# ccos-network --wifi-again: restart only Wi-Fi (the USB stick with your saved
# Wi-Fi networks was found after boot)
if [ "$1" = --wifi-again ]; then
    killall -q wpa_cli wpa_supplicant 2>/dev/null; usleep 300000
fi

# ---- cable ----
if [ "$1" != --wifi-again ] && ip link show eth0 >/dev/null 2>&1 && [ "$NET" = 1 ]; then
    ip link set eth0 up
    udhcpc -i eth0 -s /usr/share/udhcpc/default.script -t 5 -T 2 -A 5 -b -p /run/udhcpc-eth0.pid >/dev/null 2>&1
fi

# ---- Wi-Fi ----
# a Wi-Fi card appears a moment after its firmware has loaded: wait for it briefly
if ls /sys/bus/pci/drivers/iwlwifi/0000:* >/dev/null 2>&1; then
    for i in 1 2 3 4 5 6 7 8 9 10; do [ -d /sys/class/net/wlan0 ] && break; usleep 300000; done
fi
[ -d /sys/class/net/wlan0 ] && [ -x /sbin/wpa_supplicant ] || exit 0
for f in /sys/class/rfkill/rfkill*/soft; do [ -f "$f" ] && echo 0 > "$f"; done    # un-block the radio
if mountpoint -q /data; then mkdir -p /data/wifi; WCONF=/data/wifi/wpa_supplicant.conf
else WCONF=/tmp/wpa_supplicant.conf; fi
if [ ! -f "$WCONF" ]; then
    printf 'ctrl_interface=/run/wpa_supplicant\nupdate_config=1\ncountry=PL\n' > "$WCONF"
fi
chmod 600 "$WCONF"
# what happened while connecting (wrong password, router said no...): the desktop reads it,
# and it's in D:/wifi/wifi-log.txt for checking by hand
WLOG=/tmp/wifi-log.txt; mountpoint -q /data && WLOG=/data/wifi/wifi-log.txt
[ -f "$WLOG" ] && tail -c 20000 "$WLOG" > "$WLOG.old" 2>/dev/null; : > "$WLOG"
grep -q '^sae_pwe=' "$WCONF" || echo 'sae_pwe=2' >> "$WCONF"          # WPA3: both ways of using the password
wpa_supplicant -B -t -D nl80211 -i wlan0 -c "$WCONF" -f "$WLOG" >/dev/null 2>&1
for i in 1 2 3 4 5 6 7 8 9 10; do [ -S /run/wpa_supplicant/wlan0 ] && break; usleep 100000; done
wpa_cli -p /run/wpa_supplicant -i wlan0 -a /usr/share/wifi/action.sh -B >/dev/null 2>&1      # gets an address on connect
if [ "$NET" = 0 ] || [ "$WIFI" = 0 ]; then                          # Wi-Fi or networking switched off
    wpa_cli -p /run/wpa_supplicant -i wlan0 disconnect >/dev/null 2>&1
    ip link set wlan0 down
fi
exit 0
