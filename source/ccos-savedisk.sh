#!/bin/sh
# ccos-savedisk [--watch] - find ccos's save disk and mount it at /data
#   in QEMU:            the virtual disk /dev/vda (ccos-data.img next to run.bat)
#   from the USB stick: the stick's second partition "ccos-store" (made from
#                       the empty rest of the stick the first time ccos starts):
#                       /data (D:, settings, files) and /apps (installed apps,
#                       Downloads) both live there.
#                       Sticks written in Rufus's ISO mode have no empty space;
#                       they keep the older 128 MB file ccos/ccos-data.img.
# ccos never touches the computer's own drive: its kernel has no driver for
# NVMe/SATA disks, so only USB drives are visible. Other USB drives are only
# looked at read-only; only the stick with ccos/ccos.txt on it is written to.
#   --watch  keep looking in the background (for a stick that shows up late
#            or is plugged in again after boot)
[ "$1" = --watch ] || { mountpoint -q /data && exit 0; }
mkdir -p /data /mnt/usb

if [ -b /dev/vda ]; then
    blkid /dev/vda | grep -q 'TYPE=' || mke2fs -q -L ccos-data /dev/vda >/dev/null 2>&1
    mount /dev/vda /data 2>/dev/null
    exit $?
fi
grep -q 'ccos.live' /proc/cmdline || exit 1

# the stick's own partition for files and apps; made on the first start
use_store() {
    part=${1##*/}
    disk=$(basename "$(readlink -f /sys/class/block/$part/..)")
    [ -b "/dev/$disk" ] || return 1
    store=""
    for q in /dev/$disk*; do
        [ "$q" = "/dev/$disk" ] && continue
        blkid "$q" 2>/dev/null | grep -q 'LABEL="ccos-store"' && store=$q
    done
    if [ -z "$store" ]; then
        new=$(/sbin/ccos-mkpart "/dev/$disk" 2>/tmp/ccos-mkpart.log) || return 1
        i=0; while [ ! -b "$new" ] && [ $i -lt 20 ]; do usleep 100000; i=$((i + 1)); done
        echo "ccos: made $new for files and apps" > /dev/kmsg
        /usr/sbin/mkfs.ext4 -q -F -L ccos-store -E lazy_itable_init=1,lazy_journal_init=1 "$new" >/dev/null 2>&1 || return 1
        store=$new
    fi
    mkdir -p /mnt/store /apps
    mount -t ext4 -o noatime "$store" /mnt/store 2>/dev/null || return 1
    mkdir -p /mnt/store/data /mnt/store/apps
    if [ -f /mnt/usb/ccos/ccos-data.img ] && [ ! -f /mnt/store/data/ccos.conf ]; then
        # settings from the older save file: bring them along once
        mkdir -p /mnt/old && mount -o loop,ro /mnt/usb/ccos/ccos-data.img /mnt/old 2>/dev/null &&
            { cp -a /mnt/old/. /mnt/store/data/ 2>/dev/null; umount /mnt/old; }
    fi
    mount --bind /mnt/store/data /data && mount --bind /mnt/store/apps /apps && return 0
    umount /mnt/store 2>/dev/null
    return 1
}

# 0 = mounted, 1 = not found (yet), 2 = found but read-only (DD-mode stick)
try_once() {
    for p in /dev/sd[a-z]*; do
        [ -b "$p" ] || continue
        t=$(blkid "$p" 2>/dev/null | sed -n 's/.*TYPE="\([^"]*\)".*/\1/p')
        case "$t" in
            vfat|exfat) ;;
            iso9660) blkid "$p" | grep -q 'LABEL="CCOS"' && return 2; continue ;;
            *) continue ;;
        esac
        # mounted somewhere else already (e.g. as a USB drive under /media)? Linux
        # won't mount it a second time with other settings: take it back first
        grep -q "^$p " /proc/mounts && { grep "^$p " /proc/mounts | grep -q " /mnt/usb " || umount -l "$p" 2>/dev/null; }
        mountpoint -q /mnt/usb && umount /mnt/usb 2>/dev/null
        mount -t "$t" -o ro "$p" /mnt/usb 2>/dev/null || continue
        if [ -f /mnt/usb/ccos/ccos.txt ]; then
            mount -o remount,rw /mnt/usb 2>/dev/null || { umount /mnt/usb; return 1; }
            use_store "$p" && return 0
            F=/mnt/usb/ccos/ccos-data.img
            if [ ! -f "$F" ]; then              # first start: make a 128 MB save file
                rm -f "$F.new"
                dd if=/dev/zero of="$F.new" bs=1M count=128 2>/dev/null &&
                mke2fs -q -F -L ccos-data "$F.new" >/dev/null 2>&1 &&
                mv "$F.new" "$F"
                sync
            fi
            mount -o loop "$F" /data 2>/dev/null && return 0
            umount /mnt/usb
            return 1
        fi
        umount /mnt/usb
    done
    return 1
}

# is the stick behind /data gone (pulled out, or it dropped off USB)? Then the
# mounts are dead: let go of them so the stick can be used again when it's back
stale() {
    d=$(awk '$2=="/mnt/store" || $2=="/data" {print $1; exit}' /proc/mounts)
    case "$d" in
        /dev/sd*) [ -e "/sys/class/block/${d#/dev/}" ] && return 1; return 0 ;;
        /dev/loop*) [ -f /mnt/usb/ccos/ccos.txt ] && return 1; return 0 ;;
    esac
    return 1
}
let_go() {
    for m in /apps /data /mnt/store /mnt/usb; do umount -l "$m" 2>/dev/null; done
    echo "ccos: the USB stick went away; waiting for it to come back" > /dev/kmsg
}
found_again() {
    # restart Wi-Fi so it knows the networks saved on the stick
    pidof wpa_supplicant >/dev/null && /sbin/ccos-network --wifi-again
    mountpoint -q /apps && /sbin/ccos-apps            # installed apps are there too
    echo "ccos: using the USB stick again" > /dev/kmsg
}

if [ "$1" = --watch ]; then
    # for as long as ccos runs (from the stick): if the stick is missing - not
    # found yet at boot, or pulled out and plugged in again - take it back
    while true; do
        if mountpoint -q /data; then
            if stale; then let_go; else sleep 2; continue; fi
        fi
        try_once; r=$?
        [ $r = 0 ] && { found_again; continue; }
        [ $r = 2 ] && exit 1
        sleep 2
    done
fi

# at boot: wait about 3 seconds at most. If the stick is slower than that
# (some USB hubs take 30 s), init keeps looking in the background and the
# desktop switches to your saved settings the moment it's found.
i=0
while [ $i -lt 6 ]; do
    try_once; r=$?
    [ $r = 0 ] && exit 0
    [ $r = 2 ] && exit 1                    # read-only stick: nowhere to save
    usleep 500000
    i=$((i + 1))
done
exit 1
