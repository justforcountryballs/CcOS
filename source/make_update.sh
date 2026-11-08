#!/bin/sh
# make_update.sh - makes a signed ccos update file (.ccupd) for GitHub Releases
#   usage: make_update.sh VERSION KERNEL INITRAMFS OUT.ccupd [KEYFILE]
# The initramfs must already contain /etc/ccos-version = VERSION.
# KEYFILE: the private release key (default: ../keys/ccos-update.key). Keep it
# secret: anyone with it can make updates every ccos stick will accept.
set -e
V=$1; K=$2; I=$3; OUT=$4
D=$(cd "$(dirname "$0")" && pwd)
KEY=${5:-$D/../keys/ccos-update.key}
[ -n "$OUT" ] || { echo "usage: make_update.sh VERSION KERNEL INITRAMFS OUT.ccupd [KEYFILE]"; exit 1; }
[ -x "$D/ccos-sign" ] || gcc -O2 -o "$D/ccos-sign" "$D/ccos-sign.c" -lsodium
[ -f "$D/BOOTX64.EFI" ] || sh "$D/make_efi.sh"
W=$(mktemp -d)
cp "$K" $W/bzImage; cp "$I" $W/initramfs.cpio.gz
cp "$D/BOOTX64.EFI" $W/BOOTX64.EFI; cp "$D/stick-grub.cfg" $W/grub.cfg
{
    echo "version $V"
    for f in bzImage initramfs.cpio.gz BOOTX64.EFI grub.cfg; do
        echo "file $f $(stat -c %s $W/$f) $(sha256sum $W/$f | cut -d' ' -f1)"
    done
    echo "end"
} > $W/header
SIG=$("$D/ccos-sign" sign "$KEY" $W/header)
{ printf 'CCUPD1\nSIG %s\n' "$SIG"; cat $W/header $W/bzImage $W/initramfs.cpio.gz $W/BOOTX64.EFI $W/grub.cfg; } > "$OUT"
rm -rf $W
ls -la "$OUT"
