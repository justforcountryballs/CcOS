#!/bin/sh
# ccos-shutdown [poweroff|reboot|shell]
#   poweroff  show the shutdown screen, then power off
#   reboot    show the restart screen, then restart
#   shell     restart straight into the command line (no splash, no desktop)
MODE=poweroff
case "$1" in
    reboot) MODE=reboot ;;
    shell)  MODE=shell ;;
esac

if [ "$MODE" = shell ]; then
    if mountpoint -q /data; then
        touch /data/.boot-to-shell          # init reads this on the next boot
    else
        exit 42                             # no save disk: init drops to the shell right away
    fi
fi

# close the desktop if it's running, so the shutdown screen can take over
if killall -q desktop 2>/dev/null; then usleep 300000; fi

SP=
if [ -e /dev/fb0 ] && [ -x /bin/splash ]; then
    case "$MODE" in
        reboot) ARG=--reboot ;;
        shell)  ARG=--shell ;;
        *)      ARG=--shutdown ;;
    esac
    /bin/splash $ARG </dev/null >/dev/null 2>&1 &
    SP=$!
else
    echo; echo " ccos is shutting down..."
fi

sync                                   # write out anything still in memory
sleep 1
umount -a -r 2>/dev/null               # unmount filesystems (read-only fallback)
sync
sleep 1                                # let the spinner show for a moment

[ -n "$SP" ] && kill $SP 2>/dev/null
[ "$MODE" = shell ] && MODE=reboot
exec busybox $MODE -f
