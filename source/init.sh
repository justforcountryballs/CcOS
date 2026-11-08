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
    exec setsid cttyhack /bin/sh -l
fi

# ---- graphical splash (banner + spinning dots + progress bar) ---------
if [ -e /dev/fb0 ] && [ -x /bin/splash ]; then
    mkfifo /splash.fifo
    /bin/splash < /splash.fifo &
    SPLASH=$!
    exec 3> /splash.fifo
    progress() { echo "$1 $2" >&3; }

# ---- fallback: text-mode splash (no graphics available) ----------------
else
    set -- $(stty size 2>/dev/null)
    ROWS=${1:-25}; COLS=${2:-80}
    [ "$ROWS" -gt 0 ] 2>/dev/null || ROWS=25
    [ "$COLS" -gt 0 ] 2>/dev/null || COLS=80
    at()     { printf '%s[%d;%dH' "$E" "$1" "$2"; }
    center() { printf '%s' "$(( (COLS - $1) / 2 + 1 ))"; }
    BAR_W=40
    TOP=$(( (ROWS - 12) / 2 )); [ "$TOP" -lt 1 ] && TOP=1
    BAR_ROW=$(( TOP + 8 ))
    BAR_COL=$(center $(( BAR_W + 2 )))
    printf '%s[?25l%s[2J%s[1;36m' "$E" "$E" "$E"
    c=$(center 20); r=$TOP
    for line in '  ___ ___ ___  ___ ' ' / __/ __/ _ \/ __|' '| (_| (_| (_) \__ \' ' \___\___\___/|___/'; do
        at $r $c; printf '%s' "$line"; r=$(( r + 1 ))
    done
    printf '%s[0;37m' "$E"; at $(( TOP + 5 )) $(center 22); printf 'Linux 6.12 + BusyBox%s[0m' "$E"
    at $BAR_ROW $BAR_COL; printf '['; at $BAR_ROW $(( BAR_COL + BAR_W + 1 )); printf ']'
    progress() {
        filled=$(( $1 * BAR_W / 100 ))
        at $BAR_ROW $(( BAR_COL + 1 ))
        printf '%s[46m%*s%s[0m%*s' "$E" "$filled" '' "$E" $(( BAR_W - filled )) ''
        at $BAR_ROW $(( BAR_COL + BAR_W + 3 )); printf '%3d%%' "$1"
        at $(( BAR_ROW + 2 )) 1; printf '%s[2K' "$E"
        at $(( BAR_ROW + 2 )) $(center ${#2}); printf '%s[2m%s%s[0m' "$E" "$2" "$E"
    }
fi

# ---- boot sequence --------------------------------------------------
progress 5  "Starting ccos...";                 usleep 600000
progress 20 "Mounting /sys";                    mount -t sysfs none /sys;        usleep 500000
progress 30 "Mounting /tmp";                    mount -t tmpfs none /tmp;        usleep 400000

# the save disk holds settings: ccos-data.img in the ccos folder (QEMU),
# or ccos/ccos-data.img on the USB stick (real computer)
if grep -q 'ccos.live' /proc/cmdline; then
    progress 45 "Looking for the ccos USB stick"
else
    progress 45 "Mounting save disk"
fi
if ! /sbin/ccos-savedisk && grep -q 'ccos.live' /proc/cmdline; then
    /sbin/ccos-savedisk --watch &       # not found yet: keep looking in the background
fi
usleep 300000

# "Boot Into Shell" when running from the USB stick (its save file is only
# found now, after the splash started)
if [ -f /data/.boot-to-shell ]; then
    rm -f /data/.boot-to-shell; sync
    if [ -n "$SPLASH" ]; then echo quit >&3; wait $SPLASH; exec 3>&-; rm -f /splash.fifo; fi
    [ -x /bin/splash ] && /bin/splash --text
    hostname ccos
    [ -x /etc/rc.local ] && /etc/rc.local
    /sbin/ccos-network &
    printf '%s[0m%s[2J%s[H%s[?25h' "$E" "$E" "$E" "$E"
    exec setsid cttyhack /bin/sh -l
fi
progress 55 "Preparing the apps disk";          /sbin/ccos-apps
progress 58 "Starting sound";                  /sbin/ccos-sound
progress 60 "Setting hostname";                 hostname ccos;                   usleep 500000
# your own startup commands, if you add an /etc/rc.local script
[ -x /etc/rc.local ] && /etc/rc.local
progress 80 "Connecting to the network"
/sbin/ccos-network &                 # cable + Wi-Fi, in the background so boot isn't held up
usleep 500000
progress 95 "Starting desktop";                                                  usleep 500000
progress 100 "Welcome to ccos!";                                                 usleep 800000

# ---- stop the splash ------------------------------------------------
if [ -n "$SPLASH" ]; then
    echo quit >&3
    wait $SPLASH
    exec 3>&-
    rm -f /splash.fifo
fi

# ---- start the desktop (Ctrl+Alt+Backspace quits to the shell) -------
RC=0
if [ -e /dev/fb0 ] && [ -x /bin/desktop ]; then
    /bin/desktop
    RC=$?
fi
[ -x /bin/splash ] && /bin/splash --text      # make sure text mode is back

# "Boot Into Shell" without a save disk can't survive a restart,
# so the desktop exits with code 42 and we go straight to a plain shell
if [ "$RC" = 42 ]; then
    printf '%s[0m%s[2J%s[H%s[?25h' "$E" "$E" "$E" "$E"
    exec setsid cttyhack /bin/sh -l
fi

# ---- hand over to the shell -----------------------------------------
printf '%s[0m%s[2J%s[H%s[?25h' "$E" "$E" "$E" "$E"   # reset, clear, show cursor
cat /etc/motd
echo " Type 'desktop' to go back to the desktop."
echo
exec setsid cttyhack /bin/sh -l
