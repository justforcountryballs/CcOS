#!/bin/sh
# ccos-easy - the Terminal's easy commands (each prints the usual Linux way under it; "hints off" hides that)
#   rd hi.txt               read a file
#   adf hi.txt : more text  add a line to the end of a file (\n = another line)
#   rn old.txt : new.txt    rename
#   cpf photo.png : usb1    copy (to usb1, D:, a folder, or a new name)
#   rmt hi.txt              into the Recycle Bin (on a USB stick: deleted for good, after asking)
#   op hi.txt               open in its desktop app
#   jusb usb1               eject a USB stick safely
#   space                   free space
#   net                     is the internet working?
#   whereami                where you are, in words
# Typed in the Terminal, bash.bashrc hands over the line exactly as typed (--raw), so names with
# spaces and text with ' * ; & are fine.
G=$(printf '\033[1;32m'); R=$(printf '\033[1;31m'); Y=$(printf '\033[1;33m'); C=$(printf '\033[1;36m'); D=$(printf '\033[2m'); N=$(printf '\033[0m')
HINT=/usr/bin/ccos-hint
ok()   { echo "${G}✔${N} $*"; }
bad()  { echo "${R}✖${N} $*"; }
warn() { echo "${Y}!${N} $*"; }

cmd=${0##*/}; [ "$cmd" = ccos-easy ] && { cmd=$1; shift; }
if [ "$1" = "--raw" ]; then line=$2; else line="$*"; fi
line=$(printf '%s' "$line" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')

# a name as typed -> without quotes, and Tab's "my\ file" -> "my file"
clean() {
    n=$(printf '%s' "$1" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')
    case "$n" in \"*\"|\'*\') n=${n#?}; n=${n%?} ;; *) n=$(printf '%s' "$n" | sed 's/\\\(.\)/\1/g') ;; esac
    printf '%s' "$n"
}
# a usb1 / D: name -> its folder
mapname() {
    n=$1
    case "$n" in
        D:|d:) n=/data ;; D:/*|d:/*) n=/data/${n#??/} ;;
        usb[0-9]*|USB[0-9]*)
            u=${n%%/*}; r=$(/usr/bin/ccos-usb find "$u" 2>/dev/null) && n="$(printf '%s' "$r" | cut -f2)${n#"$u"}" ;;
    esac
    printf '%s' "$n"
}
# where a name is: here, or (in the home folder) in Documents, where mkf puts files
resolve() {
    n=$(clean "$1")
    [ -z "$n" ] && return 1
    case "$n" in /*) [ -e "$n" ] || [ -L "$n" ] && { printf '%s' "$n"; return 0; } ;;
                 *)  [ -e "$PWD/$n" ] || [ -L "$PWD/$n" ] && { printf '%s' "$PWD/$n"; return 0; } ;; esac
    m=$(mapname "$n"); [ "$m" != "$n" ] && [ -e "$m" ] && { printf '%s' "$m"; return 0; }
    [ "$PWD" = "$HOME" ] && [ -e "/data/Documents/$n" ] && { printf '%s' "/data/Documents/$n"; return 0; }
    return 1
}
# "usb1 (KINGSTON)" / "D:/Documents" / the folder
place() {
    p=$1
    for u in $(/usr/bin/ccos-usb names 2>/dev/null); do
        d=$(/usr/bin/ccos-usb find "$u" 2>/dev/null | cut -f2)
        case "$p/" in "$d"/*) s=${p#"$d"}; printf '%s (%s)%s' "$u" "${d#/media/}" "${s:+, folder ${s#/}}"; return ;; esac
    done
    case "$p" in /data) printf 'D:' ;; /data/*) printf 'D:/%s' "${p#/data/}" ;; *) printf '%s' "$p" ;; esac
}
# for hints: a name the shell would take as it is
q() { case "$1" in *[!A-Za-z0-9._/+:-]*) printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")" ;; *) printf '%s' "$1" ;; esac; }
rel() { case "$1" in "$PWD/"*) printf '%s' "${1#"$PWD/"}" ;; *) printf '%s' "$1" ;; esac; }
# split at the first " : "  ->  A, B
split() {
    case "$line" in
        *" : "*) A=${line%%" : "*}; B=${line#*" : "} ;;
        *" :"*)  A=${line%%" :"*}; B=${line#*" :"} ;;
        *) return 1 ;;
    esac
    B=$(printf '%s' "$B" | sed 's/^[[:space:]]*//')
}
newlines() { printf '%s' "$1" | awk '{ gsub(/\\\\n/, "\001"); gsub(/[ \t]*\\n[ \t]*/, "\n"); gsub(/\001/, "\\n"); printf "%s", $0 }'; }
nothere() { bad "There's no file or folder called $1 here. (ls shows what's here)"; exit 1; }
free_name() {             # free_name DIR NAME -> NAME, or "NAME (2).ext" ...
    nm=$2; base=$nm; ext=""
    case "$nm" in ?*.*) ext=".${nm##*.}"; base=${nm%.*} ;; esac
    k=2; while [ -e "$1/$nm" ] || [ -L "$1/$nm" ]; do nm="$base ($k)$ext"; k=$((k + 1)); done
    printf '%s' "$nm"
}
human() { awk -v k="$1" 'BEGIN { if (k >= 1048576) printf "%.1f GB", k / 1048576; else if (k >= 1024) printf "%d MB", k / 1024; else printf "%d KB", k }'; }

case "$cmd" in
# ------------------------------------------------------------------ rd
rd)
    [ -z "$line" ] && { echo "rd shows what's inside a file:  rd hi.txt"; exit 0; }
    f=$(resolve "$line") || nothere "$(clean "$line")"
    nm=${f##*/}
    if [ -d "$f" ]; then bad "$nm is a folder, not a file. (ls $(q "$(rel "$f")") shows what's inside)"; exit 1; fi
    if [ ! -s "$f" ]; then echo "$nm is empty."; $HINT "cat $(q "$(rel "$f")")"; exit 0; fi
    a=$(head -c 4096 "$f" | wc -c); b=$(head -c 4096 "$f" | tr -d '\000' | wc -c)
    if [ "$a" != "$b" ]; then bad "$nm isn't a text file (a picture, program...). Try:  op $nm"; exit 1; fi
    lines=$(wc -l < "$f"); rows=$(stty size 2>/dev/null < /dev/tty | cut -d' ' -f1)
    if [ -t 1 ] && [ "${rows:-0}" -gt 0 ] && [ "$lines" -gt $((rows - 3)) ]; then
        less -RF "$f"; $HINT "less $(q "$(rel "$f")")  (or cat)"
    else
        cat "$f"; [ -n "$(tail -c 1 "$f")" ] && echo
        $HINT "cat $(q "$(rel "$f")")"
    fi ;;
# ------------------------------------------------------------------ adf
adf)
    if ! split || [ -z "$(clean "$A")" ]; then
        echo "adf adds text to the end of a file:  adf hi.txt : more text   (\\n starts another line)"; exit 0; fi
    f=$(resolve "$A") || { bad "There's no file called $(clean "$A"). (mkf $(clean "$A") : text makes a new one)"; exit 1; }
    [ -d "$f" ] && { bad "${f##*/} is a folder. adf adds to files."; exit 1; }
    [ -z "$B" ] && { bad "Nothing to add. Put the text after the colon:  adf ${f##*/} : more text"; exit 1; }
    t=$(newlines "$B"; printf x); t=${t%x}
    { [ -s "$f" ] && [ -n "$(tail -c 1 "$f")" ] && echo; printf '%s\n' "$t"; } >> "$f" 2>/dev/null \
        || { bad "Couldn't add to ${f##*/} (is it read-only?)."; exit 1; }
    n=$(printf '%s\n' "$t" | wc -l); [ "$n" = 1 ] && w="1 line" || w="$n lines"
    ok "Added $w to ${f##*/}"
    case "$t" in *"
"*) args=$(printf '%s\n' "$t" | while IFS= read -r l; do printf ' %s' "$(q "$l")"; done)
        $HINT "printf '%s\\n'$args >> $(q "$(rel "$f")")" ;;
    *)  $HINT "echo $(q "$t") >> $(q "$(rel "$f")")" ;; esac ;;
# ------------------------------------------------------------------ rn
rn)
    if ! split || [ -z "$(clean "$A")" ] || [ -z "$(clean "$B")" ]; then
        echo "rn renames:  rn old.txt : new.txt"; exit 0; fi
    f=$(resolve "$A") || nothere "$(clean "$A")"
    new=$(clean "$B"); dir=$(dirname "$f")
    case "$new" in */*) bad "The new name can't have a / in it. (rn only renames; cpf copies to another place)"; exit 1 ;; esac
    [ "$new" = "${f##*/}" ] && { echo "That's already its name."; exit 0; }
    if [ -e "$dir/$new" ] && [ "$(echo "$new" | tr A-Z a-z)" != "$(echo "${f##*/}" | tr A-Z a-z)" ]; then
        bad "There's already something called $new there. Try a different name."; exit 1; fi
    mv "$f" "$dir/$new" 2>/dev/null || { bad "Couldn't rename ${f##*/} (is it read-only, or in use?)."; exit 1; }
    ok "Renamed ${f##*/} to $new"
    $HINT "mv $(q "$(rel "$f")") $(q "$(rel "$dir/$new")")" ;;
# ------------------------------------------------------------------ cpf
cpf)
    if ! split || [ -z "$(clean "$A")" ] || [ -z "$(clean "$B")" ]; then
        echo "cpf copies:  cpf photo.png : usb1    (or : D:  or : a folder  or : a new name)"; exit 0; fi
    f=$(resolve "$A") || nothere "$(clean "$A")"
    to=$(clean "$B"); m=$(mapname "$to")
    case "$to" in usb[0-9]*|USB[0-9]*) [ "$m" = "$to" ] && { bad "There's no ${to%%/*}. Type lsusb to see the sticks."; exit 1; } ;; esac
    if d=$(resolve "$to") && [ -d "$d" ]; then dest="$d/${f##*/}"
    elif [ "$m" != "$to" ] && [ -d "$m" ]; then dest="$m/${f##*/}"
    else
        case "$to" in */) bad "There's no folder called ${to%/}."; exit 1 ;;
                      /*) dest=$to ;; */*) dest="$PWD/$to" ;; *) dest="$(dirname "$f")/$to" ;; esac
        [ -d "$(dirname "$dest")" ] || { bad "There's no folder called $(dirname "$to")."; exit 1; }
    fi
    [ "$dest" = "$f" ] && { bad "That's the same file. Give the copy another name, like:  cpf ${f##*/} : copy of ${f##*/}"; exit 1; }
    if [ -e "$dest" ]; then bad "There's already a ${dest##*/} in $(place "$(dirname "$dest")"). (rn it first, or copy with another name)"; exit 1; fi
    case "$dest/" in "$f"/*) bad "A folder can't be copied into itself."; exit 1 ;; esac
    r=""; [ -d "$f" ] && r="-r "
    cp -r "$f" "$dest" 2>/dev/null || { rm -rf "$dest" 2>/dev/null; bad "Couldn't copy ${f##*/} (not enough space, or read-only?)."; exit 1; }
    case "$dest" in /media/*) sync ;; esac
    as=""; [ "${dest##*/}" != "${f##*/}" ] && as=" as ${dest##*/}"
    ok "Copied ${f##*/} to $(place "$(dirname "$dest")")$as"
    $HINT "cp $r$(q "$(rel "$f")") $(q "$(rel "$dest")")" ;;
# ------------------------------------------------------------------ rmt
rmt)
    [ -z "$line" ] && { echo "rmt puts a file or folder in the Recycle Bin:  rmt hi.txt"; exit 0; }
    # one name (maybe with spaces), a pattern like *.txt, or several names
    set --
    if f=$(resolve "$line"); then set -- "$f"
    else
        n=$(clean "$line")
        case "$n" in *[*?]*)
            base=$PWD; [ "$PWD" = "$HOME" ] && ! ls -d $n >/dev/null 2>&1 && base=/data/Documents
            list=$(cd "$base" 2>/dev/null && for x in $n; do [ -e "$x" ] && printf '%s\n' "$base/$x"; done)
            old=$IFS; IFS='
'; for g in $list; do set -- "$@" "$g"; done; IFS=$old ;;
        *)  for w in $line; do g=$(resolve "$w") || nothere "$(clean "$w")"; set -- "$@" "$g"; done ;;
        esac
        [ $# = 0 ] && nothere "$n"
    fi
    for f in "$@"; do
        nm=${f##*/}
        case "$f" in
            /data|/data/|/data/.bin|/data/.bin/*|/media|/media/*/|/root|/) bad "$nm can't be deleted with rmt."; continue ;;
            /data/*|/root/*|/tmp/*|/apps/home/*|/media/*) ;;
            *) bad "$nm is part of ccos itself, so rmt won't delete it."; continue ;;
        esac
        case "$f" in /media/*/*) ;; /media/*) bad "$nm is a whole USB stick. (jusb ejects it)"; continue ;; esac
        if case "$f" in /media/*) true ;; *) ! mountpoint -q /data 2>/dev/null ;; esac; then
            printf '%s is on a USB stick, so it can'"'"'t go to the Recycle Bin. Delete it for good? [y/N] ' "$nm"
            read -r a < /dev/tty
            case "$a" in y|Y|yes|Yes|YES|t|T|tak) rm -rf "$f" && sync && ok "Deleted $nm for good." || bad "Couldn't delete $nm." ;;
                         *) echo "Kept $nm." ;; esac
            $HINT "rm $( [ -d "$f" ] && printf -- '-r ')$(q "$(rel "$f")")"
            continue
        fi
        mkdir -p /data/.bin && chmod 700 /data/.bin
        inbin=$(free_name /data/.bin "$nm")
        if mv "$f" "/data/.bin/$inbin" 2>/dev/null; then
            printf '%s\t%s\t%s\n' "$inbin" "$f" "$(date +%s)" >> /data/.bin/.index
            ok "Moved $nm to the Recycle Bin. (Open the Recycle Bin on the desktop to put it back.)"
            $HINT "rm $( [ -d "/data/.bin/$inbin" ] && printf -- '-r ')$(q "$(rel "$f")") - but that deletes it for good, no Recycle Bin"
        else bad "Couldn't move $nm to the Recycle Bin (in use, or read-only?)."; fi
    done
    [ -p /run/ccos/toast ] && printf 'B\n' > /run/ccos/toast 2>/dev/null ;;
# ------------------------------------------------------------------ op
op)
    [ -z "$line" ] && { echo "op opens a file in its app:  op photo.png   (or op google.com)"; exit 0; }
    if ! pidof desktop >/dev/null 2>&1 || [ ! -p /run/ccos/toast ]; then
        bad "The desktop isn't running, so op can't open apps. (rd shows text files)"; exit 1; fi
    n=$(clean "$line")
    if ! f=$(resolve "$line"); then
        case "$n" in http://*|https://*) f=$n ;;
            *.com|*.pl|*.org|*.net|*.io|*.dev|*.eu|*.com/*|*.pl/*|*.org/*|*.net/*|www.*) f="https://$n" ;;
            *) nothere "$n" ;; esac
    fi
    xdg-open "$f" 2>/dev/null || { bad "Couldn't open ${f##*/}$(case "$f" in http*) echo " (install Firefox from Software Manager for web pages)" ;; esac)."; exit 1; }
    ok "Opening ${f##*/}"
    $HINT "xdg-open $(q "$(rel "$f")")" ;;
# ------------------------------------------------------------------ jusb
jusb)
    r=$(/usr/bin/ccos-usb find "$(clean "$line")") || exit 1
    u=$(printf '%s' "$r" | cut -f1); m=$(printf '%s' "$r" | cut -f2); nm=$(printf '%s' "$r" | cut -f3)
    dev=$(awk -v m="$m" '{ d=$1; p=$2; gsub(/\\040/, " ", p) } p==m { print d; exit }' /proc/mounts)
    sync
    if ! umount "$m" 2>/dev/null; then
        bad "Can't eject $u yet: a program is still using it. Close it (or leave its folder) and try again."; exit 1; fi
    rmdir "$m" 2>/dev/null
    disk=$(echo "${dev#/dev/}" | sed 's/[0-9]*$//')
    # its other parts too
    awk -v d="/dev/$disk" 'index($1, d) == 1 { p=$2; gsub(/\\040/, " ", p); print p }' /proc/mounts | while IFS= read -r o; do umount "$o" 2>/dev/null; rmdir "$o" 2>/dev/null; done
    [ -n "$disk" ] && [ -e "/sys/block/$disk/device/delete" ] && echo 1 > "/sys/block/$disk/device/delete" 2>/dev/null
    ok "$u ($nm) is safe to pull out now."
    $HINT "sync; umount $(q "$m")" ;;
# ------------------------------------------------------------------ space
space)
    bar() {         # bar USED_KB TOTAL_KB
        awk -v u="$1" -v t="$2" -v g="$G" -v y="$Y" -v r="$R" -v n="$N" -v dd="$D" 'BEGIN {
            p = t > 0 ? u / t : 0; k = int(p * 20 + 0.5); c = p > 0.9 ? r : p > 0.7 ? y : g
            a = ""; b = ""; for (i = 0; i < 20; i++) { if (i < k) a = a "■"; else b = b "■" }
            printf "%s%s%s%s%s%s %3d%% used", c, a, n, dd, b, n, p * 100 }'
    }
    row() {         # row NAME FOLDER NOTE
        set -- "$1" "$2" "$3" $(stat -f -c '%b %f %S' "$2" 2>/dev/null)
        tot=$(( ${4:-0} * ${6:-0} / 1024 )); fr=$(( ${5:-0} * ${6:-0} / 1024 ))
        [ "$tot" -gt 0 ] || return
        printf "  ${C}%-6s${N} %s  %s free of %s  ${D}%s${N}\n" "$1" "$(bar $((tot - fr)) $tot)" "$(human $fr)" "$(human $tot)" "$3"
    }
    echo "Free space:"
    if mountpoint -q /data 2>/dev/null; then row "D:" /data "your files"; else echo "  D: isn't found (no ccos stick), files go to memory"; fi
    mountpoint -q /apps 2>/dev/null && row "Apps" /apps "apps from Software Manager"
    for u in $(/usr/bin/ccos-usb names 2>/dev/null); do
        r=$(/usr/bin/ccos-usb find "$u" 2>/dev/null); row "$u" "$(printf '%s' "$r" | cut -f2)" "$(printf '%s' "$r" | cut -f3)"
    done
    set -- $(free | awk '/^Mem/ { print $2, $7 }')
    [ -n "$1" ] && printf "  ${C}%-6s${N} %s  %s free of %s  ${D}%s${N}\n" "Memory" "$(bar $(($1 - $2)) $1)" "$(human $2)" "$(human $1)" "(RAM, for running programs)"
    $HINT "df -h  (and free -h for memory)" ;;
# ------------------------------------------------------------------ net
net)
    via=""; name=""; ip=""
    if [ -d /sys/class/net/eth0 ] && [ "$(cat /sys/class/net/eth0/carrier 2>/dev/null)" = 1 ]; then
        a=$(ip -4 -o addr show dev eth0 2>/dev/null | awk '{ print $4 }' | cut -d/ -f1 | head -1)
        [ -n "$a" ] && { via="a cable"; ip=$a; }
    fi
    if [ -z "$via" ] && [ -d /sys/class/net/wlan0 ]; then
        st=$(wpa_cli -p /run/wpa_supplicant -i wlan0 status 2>/dev/null)
        state=$(printf '%s\n' "$st" | sed -n 's/^wpa_state=//p'); name=$(printf '%s\n' "$st" | sed -n 's/^ssid=//p')
        a=$(ip -4 -o addr show dev wlan0 2>/dev/null | awk '{ print $4 }' | cut -d/ -f1 | head -1)
        if [ "$state" = COMPLETED ] && [ -n "$a" ]; then via="Wi-Fi ($name)"; ip=$a
        elif [ "$state" = COMPLETED ]; then
            warn "Connected to the Wi-Fi $name, but it hasn't given ccos an address yet. Wait a few seconds and try net again."
            $HINT "ip a  (addresses) and ping -c 3 google.com"; exit 1
        fi
    fi
    if [ -z "$via" ]; then
        if [ -d /sys/class/net/wlan0 ]; then
            bad "Not connected. Pick a Wi-Fi network from the Wi-Fi icon at the bottom right (or plug in a cable)."
        elif [ -d /sys/class/net/eth0 ]; then bad "Not connected. Plug in a network cable."
        else bad "Not connected: ccos didn't find a network card (Wi-Fi or cable) in this computer."; fi
        $HINT "ip a  (addresses) and ping -c 3 google.com"; exit 1
    fi
    if ping -c 1 -W 3 1.1.1.1 >/dev/null 2>&1 || ping -c 1 -W 3 8.8.8.8 >/dev/null 2>&1 || nslookup google.com >/dev/null 2>&1; then
        if nslookup google.com >/dev/null 2>&1 || ping -c 1 -W 3 google.com >/dev/null 2>&1; then
            ok "Connected to the internet by $via. This computer's address is $ip."
        else
            warn "Connected by $via, and the internet answers, but names (like google.com) don't work. The router's DNS may be down."
        fi
    else
        bad "Connected by $via (address $ip), but the internet doesn't answer. The router may be offline, or the network needs a login page (open Firefox)."
    fi
    $HINT "ping -c 3 google.com  (and ip a for addresses)" ;;
# ------------------------------------------------------------------ whereami
whereami)
    p=$PWD; u=""
    for x in $(/usr/bin/ccos-usb names 2>/dev/null); do
        d=$(/usr/bin/ccos-usb find "$x" 2>/dev/null | cut -f2); case "$p/" in "$d"/*) u="$x (${d#/media/})"; s=${p#"$d"} ;; esac
    done
    if [ -n "$u" ]; then echo "You're on the USB stick $u${s:+, in the folder ${s#/}}."
    else case "$p" in
        "$HOME") echo "You're in the Terminal's home folder (~). It isn't kept after a restart, so mkf puts new files in Documents (D:/Documents)." ;;
        /data) echo "You're in D:, where your files are kept (on the ccos stick)." ;;
        /data/.bin|/data/.bin/*) echo "You're in the Recycle Bin (D:/.bin)." ;;
        /data/Documents) echo "You're in your Documents (D:/Documents), kept on the ccos stick." ;;
        /data/*) echo "You're in D:/${p#/data/}, kept on the ccos stick." ;;
        /apps/home/Downloads|/apps/home/Downloads/*) echo "You're in Downloads${p#/apps/home/Downloads}, kept on the ccos stick." ;;
        /apps|/apps/*) echo "You're on the apps disk (where Software Manager keeps apps)." ;;
        /tmp|/tmp/*) echo "You're in a temporary folder: everything here is gone after a restart." ;;
        /media) echo "You're in /media, where USB sticks show up. (lsusb lists them)" ;;
        /) echo "You're at the very top of ccos (the root folder, /). Everything is inside here." ;;
        "$HOME"/*) echo "You're in ${p#"$HOME"/}, inside the Terminal's home folder. It isn't kept after a restart." ;;
        *) echo "You're in $p, one of ccos's own folders. Better not to change things here." ;;
    esac; fi
    echo "${D}  Full path: $p${N}"
    $HINT "pwd" ;;
*)
    echo "ccos-easy: rd adf rn cpf rmt op jusb space net whereami"; exit 1 ;;
esac
