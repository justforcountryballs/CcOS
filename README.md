# CcOS

A hobby operating system on the Linux kernel, with its own desktop written from
scratch in C. It looks and works like a classic desktop: start menu, taskbar,
windows, File Explorer, Notepad, Settings, Photos/Videos/Music, Solitaire and
Minesweeper. It runs real Linux apps from Flathub (Firefox, GIMP, LibreOffice...)
in its own windows.

It runs in QEMU (`run.bat`) or from a USB stick on a real PC, with your files
and apps kept on the stick.

## Getting it

Go to **[Releases](../../releases)**:

- `ccos-usb.img.gz`: write it to a USB stick (8 GB or more) with [Rufus](https://rufus.ie)
  and start the PC from the stick (UEFI)
- `ccos.zip`: the QEMU version; unzip and run `run.bat`
- `ccos-update-X.ccupd`: the update that sticks download and install themselves
  (Settings > Update)

How to use it: [docs/README.txt](docs/README.txt).

## Features

- Desktop with Classic (glass) and Modern (flat) styles, dark mode
- Polish letters and the Polish (programmer's) keyboard
- File Explorer: copy/cut/paste, drag and drop, a selection box, zip files, USB drives
- Wi-Fi, Bluetooth, sound, printing over Wi-Fi, battery and brightness, time zones
- Flathub apps through Software Manager, plus X11 apps and games via Xwayland
- Updates from this repository's releases: signed, installed next to the old
  version, and rolled back by themselves if the new one doesn't start

## Source

`source/` holds the desktop (`desktop.c` and the files it includes), the helpers
(`files.c` copy/zip, `update.c` updater, `print.c`, `notifyd.c`...), the boot
scripts and the kernel config. The desktop builds with:

    gcc -O2 -o desktop desktop.c -lm -lwayland-server -lxkbcommon

Release tools: `make_release.sh VERSION` builds the stick image, the QEMU zip
and the signed update. Updates are signed with a private key that is **not** in
this repository.

Third-party software and licences: [source/THIRD-PARTY.txt](source/THIRD-PARTY.txt).
