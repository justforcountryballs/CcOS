#!/bin/sh
# make_efi.sh - builds GRUB for the ccos USB stick (tools/BOOTX64.EFI).
# Built once and kept, so updates only replace it when GRUB really changes.
set -e
D=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d)
cat > $W/embed.cfg <<'CFG'
search --no-floppy --file --set=root /ccos/ccos.txt
set prefix=($root)/boot/grub
configfile ($root)/boot/grub/grub.cfg
CFG
grub-mkstandalone -O x86_64-efi -o "$D/BOOTX64.EFI" \
    --modules="part_gpt part_msdos fat exfat iso9660 search search_fs_file configfile linux all_video gfxterm normal echo reboot halt loadenv test" \
    --locales="" --themes="" --fonts="" "boot/grub/grub.cfg=$W/embed.cfg"
rm -rf $W
ls -la "$D/BOOTX64.EFI"
