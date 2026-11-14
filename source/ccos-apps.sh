#!/bin/sh
# ccos-apps - get the apps disk ready for Flatpak (run at boot)
#   In QEMU this is ccos-apps.qcow2 (/dev/vdb): it only grows as apps are added.
#   Flatpak keeps apps in /var/lib/flatpak, which points at /apps/flatpak.
mkdir -p /apps /var/lib
if [ -b /dev/vdb ] && ! mountpoint -q /apps; then
    if ! blkid /dev/vdb | grep -q 'TYPE='; then          # brand-new disk: make a filesystem
        mkfs.ext4 -q -F -L ccos-apps /dev/vdb >/dev/null 2>&1
    fi
    mount -t ext4 /dev/vdb /apps 2>/dev/null || mount /dev/vdb /apps 2>/dev/null
fi
if mountpoint -q /apps; then
    mkdir -p /apps/flatpak /apps/home /apps/tmp
    chmod 1777 /apps/tmp
    rm -rf /var/lib/flatpak /var/tmp
    ln -s /apps/flatpak /var/lib/flatpak
    ln -s /apps/tmp /var/tmp                             # big downloads go here, not into memory
    C=/apps/flatpak/repo/config                          # slow stick: wait longer for Flatpak's repo lock
    [ -f $C ] && ! grep -q '^lock-timeout-secs' $C && sed -i '/^\[core\]/a lock-timeout-secs=600' $C
else
    mkdir -p /var/lib/flatpak /var/tmp                  # no apps disk: apps live in memory until restart
    chmod 1777 /var/tmp
fi
# things Flatpak and D-Bus expect on a Linux system
[ -s /etc/machine-id ] || cat /proc/sys/kernel/random/uuid | tr -d '-' > /etc/machine-id
mkdir -p /var/lib/dbus; ln -sf /etc/machine-id /var/lib/dbus/machine-id
# the system message bus (Flatpak and some apps look for it)
if [ -x /usr/bin/dbus-daemon ]; then
    mkdir -p /run/dbus
    [ -e /var/run ] || ln -s /run /var/run
    dbus-daemon --system --fork 2>/dev/null
fi
# Bluetooth (headphones, mice, keyboards): the BlueZ service, if this computer has Bluetooth
if [ -x /usr/libexec/bluetooth/bluetoothd ]; then
    if [ -w /data ] && [ "$(stat -c %d /data)" != "$(stat -c %d /)" ]; then   # remember paired devices on the save disk
        mkdir -p /data/bluetooth; rm -rf /var/lib/bluetooth; ln -s /data/bluetooth /var/lib/bluetooth
    fi
    mkdir -p /var/lib/bluetooth; chmod 700 /var/lib/bluetooth/
    /usr/libexec/bluetooth/bluetoothd -n </dev/null >/tmp/bluetoothd.log 2>&1 &
fi
exit 0
