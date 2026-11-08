#!/bin/sh
# make_usbimg.sh - builds ccos-usb.img, the image Rufus writes to a USB stick
#   usage: make_usbimg.sh KERNEL INITRAMFS OUT.img
# The image has one 512 MB FAT32 partition with ccos (UEFI boot). The rest of
# the stick stays empty until ccos's first start, when ccos turns it into a
# second partition ("ccos-store") for your files and apps.
set -e
K=$1; I=$2; OUT=$3
W=$(mktemp -d)
FATMB=512
mkdir -p $W/f/boot/ccos $W/f/boot/grub $W/f/ccos $W/f/EFI/BOOT
cp "$K" $W/f/boot/ccos/bzImage
cp "$I" $W/f/boot/ccos/initramfs.cpio.gz
cat > $W/f/ccos/ccos.txt <<'TXT'
This is a ccos USB stick.
ccos itself is on this small part of the stick. Your settings, files and
installed apps are kept on the rest of the stick (a second part called
"ccos-store", made the first time ccos starts). Windows may not show it
or may offer to format it: say no, or your ccos files are gone.
TXT
cp "$(dirname "$0")/stick-grub.cfg" $W/f/boot/grub/grub.cfg
# GRUB's settings block (which copy of ccos to start), exactly 1 KB
python3 -c "import sys; b=b'# GRUB Environment Block\\nslot=a\\n'; sys.stdout.buffer.write(b+b'#'*(1024-len(b)))" > $W/f/boot/grub/grubenv
[ -f "$(dirname "$0")/BOOTX64.EFI" ] || sh "$(dirname "$0")/make_efi.sh"
cp "$(dirname "$0")/BOOTX64.EFI" $W/f/EFI/BOOT/BOOTX64.EFI
# the FAT32 partition, filled with mtools (no loop devices needed)
truncate -s ${FATMB}M $W/fat.img
mkfs.vfat -F 32 -n CCOS $W/fat.img >/dev/null
mcopy -s -i $W/fat.img $W/f/* ::/
# the whole image: 1 MB for the partition table, then the partition
truncate -s $((FATMB + 1))M "$OUT"
dd if=$W/fat.img of="$OUT" bs=1M seek=1 conv=notrunc status=none
python3 - "$OUT" $FATMB <<'PY'
import sys, struct
out, mb = sys.argv[1], int(sys.argv[2])
mbr = bytearray(512)
mbr[440:444] = struct.pack('<I', 0x63636f73)                 # disk id "ccos"
e = bytes([0x80, 0xFE, 0xFF, 0xFF, 0x0C, 0xFE, 0xFF, 0xFF]) + struct.pack('<II', 2048, mb * 2048)
mbr[446:462] = e                                             # partition 1: FAT32 (LBA), bootable
mbr[510:512] = b'\x55\xaa'
with open(out, 'r+b') as f: f.write(mbr)
PY
rm -rf $W
ls -la "$OUT"
