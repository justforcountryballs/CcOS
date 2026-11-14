#!/bin/sh
# mkf - make a file, the easy way (ccos)
#   mkf hi.txt                   an empty hi.txt
#   mkf hi.txt : hello claude!   hi.txt with "hello claude!" inside
# In the Terminal's first folder (home) the file goes into Documents, so it's kept on your stick.
# Typed in the Terminal, everything after " : " is kept exactly as typed (bash.bashrc passes the raw line).
G=$(printf '\033[1;32m'); R=$(printf '\033[1;31m'); Y=$(printf '\033[1;33m'); N=$(printf '\033[0m')

if [ "$1" = "--raw" ]; then line=$2; else line="$*"; fi
# trim spaces at the start and end
line=$(printf '%s' "$line" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')

if [ -z "$line" ] || [ "$line" = "--help" ] || [ "$line" = "-h" ]; then
    echo "mkf makes a file."
    echo "  mkf hi.txt                   makes an empty file called hi.txt"
    echo "  mkf hi.txt : hello claude!   makes hi.txt with \"hello claude!\" inside"
    echo "  mkf list.txt : milk \\n eggs     \\n starts a new line (milk, then eggs below)"
    exit 0
fi

# split at the first " : " (or a ":" at the end, or "name :text")
case "$line" in
    *" : "*) name=${line%%" : "*}; text=${line#*" : "}; has=1 ;;
    *" :")   name=${line%" :"}; text=""; has=1 ;;
    *" :"*)  name=${line%%" :"*}; text=${line#*" :"}; has=1 ;;
    *)       name=$line; text=""; has=0 ;;
esac
name=$(printf '%s' "$name" | sed 's/[[:space:]]*$//')
# 'quoted names' and "quoted names" are fine too
case "$name" in \"*\"|\'*\') name=${name#?}; name=${name%?} ;; esac

if [ -z "$name" ]; then echo "${R}✖${N} The file needs a name, like:  mkf hi.txt"; exit 1; fi

# in the home folder (where the Terminal starts) use Documents, which is kept on the stick
case "$name" in
    /*) path=$name ;;
    *)  dir=$(pwd)
        if [ "$dir" = "$HOME" ] && [ -d /data ]; then mkdir -p /data/Documents 2>/dev/null && dir=/data/Documents; fi
        path="$dir/$name" ;;
esac

if [ -d "$path" ]; then echo "${R}✖${N} There's already a folder called $name. Try a different name."; exit 1; fi
if [ -e "$path" ]; then echo "${R}✖${N} There's already a file called $name. Try a different name."; exit 1; fi

parent=$(dirname "$path")
if [ ! -d "$parent" ]; then mkdir -p "$parent" 2>/dev/null || { echo "${R}✖${N} Couldn't make the folder $parent."; exit 1; }; fi

# \n starts a new line (spaces around it are dropped: "he \n llo" -> "he" / "llo"); \\n is a real \n
if [ -n "$text" ]; then
    text=$(printf '%s' "$text" | awk '{ gsub(/\\\\n/, "\001"); gsub(/[ \t]*\\n[ \t]*/, "\n"); gsub(/\001/, "\\n"); printf "%s", $0 }'; printf x)
    text=${text%x}
fi
if [ "$has" = 1 ] && [ -n "$text" ]; then
    printf '%s\n' "$text" > "$path" 2>/dev/null
else
    : > "$path" 2>/dev/null
fi || { echo "${R}✖${N} Couldn't make $name here (is this place read-only?)."; exit 1; }

where=$parent
case "$where" in /data) where="D:" ;; /data/*) where="D:/${where#/data/}" ;; esac
if [ -n "$CCOS_USB_DIR" ]; then case "$parent/" in "$CCOS_USB_DIR"/*)
    sub=${parent#"$CCOS_USB_DIR"}; where="$CCOS_USB ($CCOS_USB_NAME)${sub:+, folder ${sub#/}}" ;; esac; fi
if [ -n "$text" ]; then
    echo "${G}✔${N} Made the file ${name##*/} in $where (with your text inside)"
else
    echo "${G}✔${N} Made the empty file ${name##*/} in $where"
fi
case "$path" in /data/*|/mnt/*|/media/*) ;; *)
    echo "${Y}!${N} This folder isn't saved on your stick, so the file is gone after a restart. (cd D: first to keep it.)" ;;
esac
