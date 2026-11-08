#!/bin/sh
# make_iso.sh - builds ccos.iso (UEFI) from the kernel + root filesystem
#   usage: make_iso.sh KERNEL INITRAMFS OUT.iso
set -e
K=$1; I=$2; OUT=$3
W=$(mktemp -d)
mkdir -p $W/iso/boot/ccos $W/iso/boot/grub $W/iso/ccos $W/iso/EFI/BOOT
cp "$K" $W/iso/boot/ccos/bzImage
cp "$I" $W/iso/boot/ccos/initramfs.cpio.gz
cat > $W/iso/ccos/ccos.txt <<'TXT'
This is a ccos USB stick.
ccos keeps your settings and files in ccos-data.img in this folder
(it's made the first time ccos starts). Delete it to start fresh.
TXT
cat > $W/iso/boot/grub/grub.cfg <<'CFG'
set timeout=3
set default=0
insmod all_video
insmod gfxterm
set gfxpayload=keep
set menu_color_normal=light-gray/black
set menu_color_highlight=black/light-cyan
menuentry "ccos" {
    linux /boot/ccos/bzImage ccos.live i915.enable_psr=0 i915.enable_fbc=0 quiet loglevel=3 logo.nologo
    initrd /boot/ccos/initramfs.cpio.gz
}
menuentry "ccos (safe graphics: if the screen stays black)" {
    linux /boot/ccos/bzImage ccos.live nomodeset quiet loglevel=3 logo.nologo
    initrd /boot/ccos/initramfs.cpio.gz
}
menuentry "ccos (show boot messages)" {
    linux /boot/ccos/bzImage ccos.live i915.enable_psr=0 i915.enable_fbc=0 loglevel=7 ccos.verbose
    initrd /boot/ccos/initramfs.cpio.gz
}
menuentry "Restart" { reboot }
menuentry "Turn off" { halt }
CFG
# the UEFI boot program: GRUB, told to find the disk that has ccos on it
cat > $W/embed.cfg <<'CFG'
search --no-floppy --file --set=root /ccos/ccos.txt
set prefix=($root)/boot/grub
configfile ($root)/boot/grub/grub.cfg
CFG
grub-mkstandalone -O x86_64-efi -o $W/iso/EFI/BOOT/BOOTX64.EFI \
    --modules="part_gpt part_msdos fat exfat iso9660 search search_fs_file configfile linux all_video gfxterm normal echo reboot halt" \
    --locales="" --themes="" --fonts="" "boot/grub/grub.cfg=$W/embed.cfg"
# a small FAT image with the boot program, for booting from the ISO itself
dd if=/dev/zero of=$W/efiboot.img bs=1K count=8192 2>/dev/null
mkfs.vfat -n CCOSEFI $W/efiboot.img >/dev/null
mmd -i $W/efiboot.img ::/EFI ::/EFI/BOOT
mcopy -i $W/efiboot.img $W/iso/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/
xorriso -as mkisofs -o "$OUT" -iso-level 3 -V CCOS -J -joliet-long -R \
    -append_partition 2 0xef $W/efiboot.img -appended_part_as_gpt \
    -e --interval:appended_partition_2:all:: -no-emul-boot \
    $W/iso 2>&1 | grep -v "^xorriso\|^Drive\|^Media\|^libisofs\|^ISO image\|^Written\|^Writing\|^ *[0-9.]*% done" || true
rm -rf $W
ls -la "$OUT"
