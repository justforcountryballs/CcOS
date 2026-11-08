#!/bin/sh
# ccos-sound - start PulseAudio, the sound server
# Apps from Flathub (Firefox, games...) play their sound through it.
# Sound cards plugged in later (USB headsets) are added every few seconds.
export XDG_RUNTIME_DIR=/run/user/0 HOME=/root
mkdir -p /run/user/0/pulse; chmod 700 /run/user/0
cfg=/run/ccos-pulse.pa
cards() { for c in /proc/asound/card[0-9]*; do [ -d "$c" ] && echo "${c##*card}"; done; }
{
    echo ".nofail"
    for n in $(cards); do echo "load-module module-alsa-card device_id=$n"; done
    echo "load-module module-native-protocol-unix auth-anonymous=1 socket=/run/user/0/pulse/native"
    echo "load-module module-default-device-restore"
    echo "load-module module-card-restore"
    echo "load-module module-stream-restore"
    echo "load-module module-switch-on-port-available"   # headphones plugged in: use them
    echo "load-module module-switch-on-connect"          # a new USB headset: use it
    echo "load-module module-always-sink"                # no sound card: apps still work
    echo "load-module module-intended-roles"
    echo "load-module module-bluetooth-policy"           # Bluetooth headphones and speakers
    echo "load-module module-bluetooth-discover"
    echo "load-module module-suspend-on-idle timeout=3"  # let go of the sound card when quiet
} > $cfg
pulseaudio -n -F $cfg --exit-idle-time=-1 --daemonize=yes --log-target=newfile:/tmp/pulseaudio.log 2>/dev/null
# watch for new sound cards (USB)
(
    known=" $(cards | tr '\n' ' ') "
    while sleep 4; do
        for n in $(cards); do
            case "$known" in *" $n "*) continue ;; esac
            known="$known$n "
            pactl load-module module-alsa-card device_id=$n >/dev/null 2>&1
        done
    done
) </dev/null >/dev/null 2>&1 &
