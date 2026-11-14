#!/bin/sh
# ccos-usb - the USB sticks plugged in, numbered usb1, usb2, ... (used by lsusb / cdusb / xusb in the Terminal)
#   ccos-usb list          a friendly list
#   ccos-usb find NAME     NAME = usb1 / 1 / (nothing, if there's only one)  ->  "usbN<TAB>folder<TAB>name"
#   ccos-usb names         usb1 usb2 ...  (for Tab completion)
# The desktop mounts sticks under /media; without it (the boot command line) this mounts them itself.
G=$(printf '\033[1;32m'); R=$(printf '\033[1;31m'); C=$(printf '\033[1;36m'); D=$(printf '\033[2m'); N=$(printf '\033[0m')

# USB disks (sdb, sdc, ...) and their partitions that hold files
usb_parts() {
    for b in /sys/block/sd*; do
        [ -e "$b" ] || continue
        case "$(readlink -f "$b")" in */usb*) ;; *) [ "$(cat "$b/removable" 2>/dev/null)" = 1 ] || continue ;; esac
        d=${b##*/}; [ "$(cat "$b/size" 2>/dev/null)" != 0 ] || continue
        found=0
        for p in "$b/$d"*; do [ -e "$p/partition" ] && { echo "${p##*/}"; found=1; }; done
        [ $found = 0 ] && echo "$d"
    done
}
# where a device is mounted ("" if not); /proc/mounts writes spaces as \040
mnt_of() { awk -v d="/dev/$1" '$1==d { gsub(/\\040/, " ", $2); print $2; exit }' /proc/mounts; }

# without the desktop nobody mounts new sticks: do it here like the desktop does
mount_new() {
    pidof desktop >/dev/null 2>&1 && return
    for p in $(usb_parts); do
        [ -n "$(mnt_of "$p")" ] && continue
        lab=$(blkid -s LABEL -o value "/dev/$p" 2>/dev/null); [ "$lab" = ccos-store ] && continue
        base=$(printf '%s' "${lab:-USB Drive}" | tr '/' '_'); m="/media/$base"; n=2
        while [ -e "$m" ] && [ $n -lt 20 ]; do m="/media/$base $n"; n=$((n + 1)); done
        mkdir -p "$m"
        for t in vfat exfat ntfs3 ext4 iso9660; do
            case $t in vfat) o=utf8=1,shortname=mixed,flush ;; exfat) o=iocharset=utf8 ;; ntfs3) o=iocharset=utf8 ;; *) o= ;; esac
            mount -t $t ${o:+-o $o} "/dev/$p" "$m" 2>/dev/null && break
        done
        if ! mountpoint -q "$m" 2>/dev/null; then rmdir "$m" 2>/dev/null; continue; fi
        if [ -e "$m/ccos/ccos.txt" ]; then umount "$m"; rmdir "$m"; fi      # ccos's own stick: that's C: and D:
    done
}

# the numbered list: "N<TAB>folder<TAB>name", ordered by device (sdb before sdc)
sticks() {
    i=0
    for p in $(usb_parts | sort); do
        m=$(mnt_of "$p"); case "$m" in /media/*) ;; *) continue ;; esac
        i=$((i + 1))
        printf '%s\t%s\t%s\n' "$i" "$m" "${m#/media/}"
    done
}

human() { awk -v k="$1" 'BEGIN { if (k >= 1048576) printf "%.1f GB", k / 1048576; else if (k >= 1024) printf "%d MB", k / 1024; else printf "%d KB", k }'; }

case "$1" in
list)
    mount_new
    L=$(sticks)
    if [ -z "$L" ]; then
        echo "No USB sticks plugged in. (Plug one in and type lsusb again.)"
        echo "${D}The stick ccos runs from isn't listed: your files on it are D:  (cd D:)${N}"
        exit 0
    fi
    echo "USB sticks:"
    echo "$L" | while IFS='	' read -r n m name; do
        set -- $(stat -f -c '%b %f %S' "$m" 2>/dev/null)
        tot=$(( ${1:-0} * ${3:-0} / 1024 )); fr=$(( ${2:-0} * ${3:-0} / 1024 ))
        here=""; case "$PWD/" in "$m"/*) here="   ${G}<- you're here${N}" ;; esac
        ro=""; awk -v m="$m" '{ gsub(/\\040/, " ", $2) } $2==m && $4 ~ /^ro/ { f=1 } END { exit !f }' /proc/mounts && ro=" ${R}(read-only)${N}"
        printf "  ${C}usb%s${N}  %-20s %s free of %s%s%s\n" "$n" "$name" "$(human $fr)" "$(human $tot)" "$ro" "$here"
    done
    echo "${D}Type cdusb usb1 to go into one, xusb to leave it. (lsusb -a lists every USB device.)${N}"
    ;;
find)
    mount_new
    L=$(sticks); want=${2#usb}; want=${want#USB}
    if [ -z "$L" ]; then echo "${R}✖${N} No USB sticks plugged in." >&2; exit 1; fi
    if [ -z "$want" ]; then
        if [ "$(echo "$L" | wc -l)" = 1 ]; then want=1
        else echo "Which one? Type cdusb and its name, like:  cdusb usb1" >&2; "$0" list >&2; exit 1; fi
    fi
    line=$(echo "$L" | awk -F'\t' -v w="$want" '$1==w')
    if [ -z "$line" ]; then echo "${R}✖${N} There's no usb$want. Type lsusb to see the sticks." >&2; exit 1; fi
    printf 'usb%s\n' "$line"
    ;;
names)
    sticks | awk -F'\t' '{ printf "usb%s ", $1 }'
    ;;
*)
    echo "ccos-usb list | find NAME | names"; exit 1 ;;
esac
