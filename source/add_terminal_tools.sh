#!/bin/sh
# add_terminal_tools.sh ROOT : the Terminal's extra programs, from Ubuntu 24.04 packages
#   bash (+ completion), nano, htop, curl, zip, 7z, sensors, ssh/scp/sftp, python3 (+ pip, tkinter)
# On the build machine first: apt-get install nano htop curl zip 7zip lm-sensors openssh-client python3-pip python3-tk bash-completion
set -e
ROOT=$1
T=$(dirname "$0")
"$T/add_files.sh" "$ROOT" /usr/bin/bash /usr/bin/nano /usr/bin/htop /usr/bin/curl /usr/bin/zip /usr/bin/zipcloak /usr/bin/zipsplit \
    /usr/bin/sensors /usr/bin/ssh /usr/bin/scp /usr/bin/sftp /usr/bin/ssh-keygen /usr/bin/ssh-keyscan /usr/bin/ssh-add /usr/bin/ssh-agent \
    /usr/bin/python3.12 /usr/lib/7zip/7z
cp /usr/lib/7zip/7z.so "$ROOT/usr/lib/7zip/"
"$T/add_files.sh" "$ROOT" /usr/lib/7zip/7z.so
printf '#! /bin/sh\nexec /usr/lib/7zip/7z "$@"\n' > "$ROOT/usr/bin/7z"; chmod +x "$ROOT/usr/bin/7z"
ln -sf /usr/bin/bash "$ROOT/bin/bash"
ln -sf python3.12 "$ROOT/usr/bin/python3"; ln -sf python3.12 "$ROOT/usr/bin/python"
[ -f /etc/sensors3.conf ] && cp /etc/sensors3.conf "$ROOT/etc/"
cp -r /usr/share/bash-completion "$ROOT/usr/share/"
mkdir -p "$ROOT/usr/share/nano" && cp /usr/share/nano/*.nanorc "$ROOT/usr/share/nano/" 2>/dev/null || true

# terminal descriptions (htop, nano and ssh need the right one)
for t in x/xterm x/xterm-256color l/linux v/vt100 v/vt220 s/screen s/screen-256color t/tmux t/tmux-256color d/dumb; do
    f=$(ls /lib/terminfo/$t /usr/share/terminfo/$t 2>/dev/null | head -1) || true
    [ -n "$f" ] || continue
    mkdir -p "$ROOT/usr/share/terminfo/$(dirname $t)"; cp -L "$f" "$ROOT/usr/share/terminfo/$t"
done

# Python: the standard library without its tests and the old tools nobody runs here
mkdir -p "$ROOT/usr/lib/python3.12" "$ROOT/usr/lib/python3/dist-packages"
( cd /usr/lib/python3.12 && tar cf - --exclude=./test --exclude=./idlelib/idle_test --exclude=./lib2to3/tests --exclude=./tkinter/test \
    --exclude=./unittest/test --exclude=./ensurepip --exclude=./turtledemo --exclude='./config-3.12-*' --exclude=./distutils/tests . ) | ( cd "$ROOT/usr/lib/python3.12" && tar xf - )
( cd /usr/lib/python3/dist-packages && tar cf - --exclude=__pycache__ pip ) | ( cd "$ROOT/usr/lib/python3/dist-packages" && tar xf - )
for so in /usr/lib/python3.12/lib-dynload/*.so; do "$T/add_files.sh" "$ROOT" "$so"; done
mkdir -p "$ROOT/usr/share/tcltk" && cp -r /usr/share/tcltk/tcl8.6 /usr/share/tcltk/tk8.6 "$ROOT/usr/share/tcltk/"
for d in tcl8.6 tk8.6; do [ -e /usr/lib/$d ] && cp -a /usr/lib/$d "$ROOT/usr/lib/"; done
# python's own data the stdlib reads
[ -f /usr/lib/python3.12/_sysconfigdata__x86_64-linux-gnu.py ] || true
echo "terminal tools added"
