#!/bin/sh
# xdg-open THING - open a web link or a file the ccos way
#   web links (and e-mail, PDFs online) go to Firefox once it's installed;
#   files open in ccos's own apps (Photos, Videos, Music, Notepad, File Explorer).
case "$1" in
    http://*|https://*|ftp://*|mailto:*|about:*)
        if flatpak info org.mozilla.firefox >/dev/null 2>&1; then
            exec /usr/bin/ccos-flatpak-run org.mozilla.firefox "$1"
        fi
        echo "xdg-open: install Firefox from Software Manager to open web links" >&2; exit 3 ;;
    file://*) f=$(printf '%s' "${1#file://}" | sed 's/%20/ /g') ;;
    *) f=$1 ;;
esac
[ -e "$f" ] || { echo "xdg-open: $f: not found" >&2; exit 2; }
case "$f" in /*) ;; *) f="$PWD/$f" ;; esac
printf 'O %s\n' "$f" > /run/ccos/toast
