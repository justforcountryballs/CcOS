#!/bin/sh
# ccos-hint TEXT - shows how the same thing is done the usual Linux way, in grey (the Terminal's
# easy commands use it). "hints off" in the Terminal turns these off, "hints on" back on.
[ -e /data/.ccos-hints-off ] || [ -e /tmp/.ccos-hints-off ] && exit 0
printf '\033[2m  (the usual Linux way: %s)\033[0m\n' "$*"
