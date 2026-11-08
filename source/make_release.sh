#!/bin/sh
# make_release.sh - everything for a new ccos version, ready for GitHub Releases
#   usage: make_release.sh VERSION      (run from the myos folder)
# Makes release/VERSION/ with:
#   ccos-update-VERSION.ccupd   the update (sticks install this by themselves)
#   ccos-usb.img.gz             a fresh stick image (Rufus)
#   ccos.zip                    the QEMU version
# Then on github.com/justforcountryballs/CcOS: Releases > Draft a new release,
# tag vVERSION, write what's new, drag the three files in, Publish.
set -e
V=$1; [ -n "$V" ] || { echo "usage: make_release.sh VERSION"; exit 1; }
D=$(cd "$(dirname "$0")/.." && pwd); cd "$D"
echo "$V" > rootfs/etc/ccos-version
(cd rootfs && find . -print0 | cpio --null -o -H newc --owner=0:0 2>/dev/null | gzip -6) > ccos/initramfs.cpio.gz
R=release/$V; mkdir -p $R
sh tools/make_update.sh "$V" ccos/bzImage ccos/initramfs.cpio.gz $R/ccos-update-$V.ccupd
sh tools/make_usbimg.sh ccos/bzImage ccos/initramfs.cpio.gz $R/ccos-usb.img >/dev/null
gzip -9 -f $R/ccos-usb.img
rm -f $R/ccos.zip; zip -qr $R/ccos.zip ccos -x 'ccos/ccos-data.img' 'ccos/ccos-apps.qcow2'
ls -la $R
