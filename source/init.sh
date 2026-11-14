#!/bin/sh
# ccos init - the first program the kernel runs (PID 1)
export PATH=/bin:/sbin:/usr/bin:/usr/sbin

mount -t proc none /proc

# ---- move into a normal memory filesystem -------------------------------
# The kernel starts us from its built-in "rootfs", which app sandboxes
# (Flatpak's bubblewrap) can't work with. So copy ccos into a tmpfs once
# and continue from there; the old copy is freed straight away.
if grep -q '^rootfs / rootfs' /proc/mounts && [ ! -e /.ccos-moved ]; then
    mkdir -p /newroot
    mount -t tmpfs -o mode=755 tmpfs /newroot
    for d in /* /.[!.]*; do
        case "$d" in /proc|/newroot|'/.[!.]*') ;; *) cp -a "$d" /newroot/ 2>/dev/null ;; esac
    done
    mkdir -p /newroot/proc /newroot/dev /newroot/sys /newroot/tmp
    : > /newroot/.ccos-moved
    umount /proc
    exec switch_root /newroot /init
fi

mount -t devtmpfs none /dev     # needed early: the screen lives at /dev/fb0
mkdir -p /dev/pts /dev/shm
mount -t devpts devpts /dev/pts  # terminals (the Terminal app needs these)
mount -t tmpfs -o mode=1777 none /dev/shm
dmesg -n 1                      # silence kernel messages on the screen

E=$(printf '\033')
SH=/bin/sh; [ -x /bin/bash ] && SH=/bin/bash     # the command line: bash (Tab, history, help)

# ---- the save disk, mounted early so we can read boot requests --------
mkdir -p /data
if [ -b /dev/vda ] && blkid /dev/vda | grep -q 'TYPE='; then
    mount /dev/vda /data 2>/dev/null
fi

# ---- "Boot Into Shell": plain command line, no splash, no desktop ------
# (left on the save disk by the start menu button; used once)
if [ -f /data/.boot-to-shell ]; then
    rm -f /data/.boot-to-shell; sync
    mount -t sysfs none /sys
    mount -t tmpfs none /tmp
    hostname ccos
    [ -x /etc/rc.local ] && /etc/rc.local
    /sbin/ccos-network &
    printf '%s[0m%s[2J%s[H%s[?25h' "$E" "$E" "$E" "$E"
    exec setsid cttyhack $SH -l
fi

# ---- the boot messages:  [  OK  ] Did something  /  [ FAIL ] Something --------
# (like most Linux systems; no picture, just what's happening)
mount -t sysfs none /sys
W=$(cut -d, -f1 /sys/class/graphics/fb0/virtual_size 2>/dev/null)          # a bigger font on big screens
if [ "${W:-0}" -ge 2560 ]; then setfont /usr/share/consolefonts/TerminusBold32x16.psf 2>/dev/null
elif [ "${W:-0}" -ge 1600 ]; then setfont /usr/share/consolefonts/TerminusBold24x12.psf 2>/dev/null; fi
printf '%s[0m%s[2J%s[H%s[?25l' "$E" "$E" "$E" "$E"           # clear the screen, hide the cursor
G="$E[1;32m"; R="$E[1;31m"; Y="$E[1;33m"; C="$E[1;36m"; N="$E[0m"
ok()   { printf '[  %sOK%s  ] %s\n' "$G" "$N" "$1"; }
fail() { printf '[ %sFAIL%s ] %s\n' "$R" "$N" "$1"; FAILED=1; }
warn() { printf '[ %sWAIT%s ] %s\n' "$Y" "$N" "$1"; }
step() { m=$1; shift; if "$@" >/dev/null 2>&1; then ok "$m"; else fail "$m"; fi; }   # run it, say how it went
printf '\n%sccos%s %s (Linux %s)\n\n' "$C" "$N" "$(cat /etc/ccos-version 2>/dev/null)" "$(uname -r)"
ok "Mounted /proc and /dev"
ok "Mounted /sys"
step "Mounted /tmp" mount -t tmpfs none /tmp

# the save disk holds settings: ccos-data.img in the ccos folder (QEMU),
# or ccos/ccos-data.img on the USB stick (real computer)
if grep -q 'ccos.live' /proc/cmdline; then
    if /sbin/ccos-savedisk; then ok "Found the ccos USB stick (your files and apps)"
    else warn "Looking for the ccos USB stick (still looking...)"; fi
    /sbin/ccos-savedisk --watch &       # not found yet, or pulled out later: keep looking
else
    if /sbin/ccos-savedisk; then ok "Mounted the save disk"; else fail "Mounted the save disk (nothing will be saved)"; fi
fi

# "Boot Into Shell" when running from the USB stick (its save file is only found now)
if [ -f /data/.boot-to-shell ]; then
    rm -f /data/.boot-to-shell; sync
    hostname ccos
    [ -x /etc/rc.local ] && /etc/rc.local
    /sbin/ccos-network >/dev/null 2>&1 &
    ok "Booting into the shell"
    printf '%s[?25h\n' "$E"
    exec setsid cttyhack $SH -l
fi
step "Prepared the apps disk"            /sbin/ccos-apps
step "Started sound"                     /sbin/ccos-sound
step "Set the computer's name (ccos)"    hostname ccos
if [ -x /etc/rc.local ]; then step "Ran your startup commands (/etc/rc.local)" /etc/rc.local; fi
/sbin/ccos-network >/dev/null 2>&1 &     # cable + Wi-Fi, in the background so boot isn't held up
ok "Started the network (it connects in the background)"
if [ -e /dev/fb0 ] && [ -x /bin/desktop ]; then ok "Starting the desktop"; else fail "Starting the desktop (no screen, or it's missing)"; fi
COLS=$(stty size 2>/dev/null | cut -d' ' -f2); [ "${COLS:-0}" -gt 20 ] 2>/dev/null || COLS=80
printf '\n%*s%sWelcome to CcOS!%s\n' $(( (COLS - 16) / 2 )) '' "$C" "$N"
usleep 900000

# ---- start the desktop (Ctrl+Alt+Backspace quits to the shell) -------
RC=1
if [ -e /dev/fb0 ] && [ -x /bin/desktop ]; then
    /bin/desktop
    RC=$?
fi
[ -x /bin/splash ] && /bin/splash --text      # make sure text mode is back

# "Boot Into Shell" without a save disk can't survive a restart,
# so the desktop exits with code 42 and we go straight to a plain shell
if [ "$RC" = 42 ]; then
    printf '%s[0m%s[2J%s[H%s[?25h' "$E" "$E" "$E" "$E"
    exec setsid cttyhack $SH -l
fi

# ---- the desktop didn't start, or crashed: stay here, with a command line ----
if [ "$RC" != 0 ]; then
    printf '%s[0m%s[?25h\n' "$E" "$E"
    fail "The desktop stopped (error $RC)"
    [ -s /tmp/ccos-crash.txt ] && echo "         What happened is in /tmp/ccos-crash.txt  (cat /tmp/ccos-crash.txt)"
    echo "         Type 'desktop' to try again, 'help' for commands, 'reboot' to restart."
    echo
    CCOS_NO_MOTD=1 exec setsid cttyhack $SH -l
fi

# ---- you quit the desktop (Ctrl+Alt+Backspace): the shell ------------
printf '%s[0m%s[2J%s[H%s[?25h' "$E" "$E" "$E" "$E"   # reset, clear, show cursor
[ "$SH" = /bin/sh ] && cat /etc/motd                  # (bash shows it itself)
echo " Type 'desktop' to go back to the desktop."
echo
exec setsid cttyhack $SH -l
