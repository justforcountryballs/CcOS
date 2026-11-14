#!/bin/sh
# ccos-fp - run a flatpak command for the Software Manager / Themes app.
#   If it fails while another Flatpak download is going (for example one
#   started in Terminal), wait for that one to finish and try again.
C=/var/lib/flatpak/repo/config
if [ -f "$C" ] && ! grep -q '^lock-timeout-secs' "$C"; then        # a slow USB stick: wait longer for the repo lock
    sed -i '/^\[core\]/a lock-timeout-secs=600' "$C" 2>/dev/null
fi
others() { pgrep -f '[f]latpak (install|update|uninstall|remote-ls|repair)' | grep -v "^$$\$" | grep -vx "$PPID"; }
n=0
while :; do
    "$@" && exit 0
    rc=$?; n=$((n + 1))
    [ $n -ge 3 ] && exit $rc
    if others >/dev/null; then
        echo "@@WAIT another Flatpak download is running; waiting for it to finish"
        t=0; while others >/dev/null && [ $t -lt 3600 ]; do sleep 3; t=$((t + 3)); done
    else
        sleep 5
    fi
    echo "@@RETRY"
done
