#!/bin/sh
# ccos-flatpak-run APP-ID - start an app installed with Flatpak
# Firefox: the first time, let it make its profile quietly in the background
# and switch off the first-run welcome screens (they get stuck on ccos).
app=$1; shift
ff_profiles() {    # Firefox's profile folders (the ones with a prefs.js)
    for f in "$HOME"/.var/app/org.mozilla.firefox/config/mozilla/firefox/*/prefs.js \
             "$HOME"/.var/app/org.mozilla.firefox/.mozilla/firefox/*/prefs.js; do
        [ -f "$f" ] && dirname "$f"
    done
}
ff_stop() {        # stop every Firefox process (also the hidden helpers)
    pkill -f /app/lib/firefox/ 2>/dev/null
    i=0
    while pgrep -f /app/lib/firefox/ >/dev/null 2>&1 && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done
    pkill -9 -f /app/lib/firefox/ 2>/dev/null
    sleep 1
}
if [ "$app" = org.mozilla.firefox ]; then
    need=1
    for p in $(ff_profiles); do [ -f "$p/user.js" ] && need=0; done
    if [ $need = 1 ]; then
        pgrep -f /app/lib/firefox/ >/dev/null 2>&1 && ff_stop    # a half-finished first start
        flatpak run org.mozilla.firefox --headless about:blank >/dev/null 2>&1 &
        i=0
        while [ $i -lt 90 ] && [ -z "$(ff_profiles)" ]; do sleep 1; i=$((i + 1)); done
        sleep 3
        ff_stop
        for p in $(ff_profiles); do
            cat > "$p/user.js" <<'PREFS'
// set by ccos: skip the first-run screens
user_pref("browser.aboutwelcome.enabled", false);
user_pref("browser.preonboarding.enabled", false);
user_pref("termsofuse.bypassNotification", true);
user_pref("datareporting.policy.dataSubmissionPolicyBypassNotification", true);
user_pref("browser.shell.checkDefaultBrowser", false);
PREFS
        done
    fi
fi
# the look of apps (Settings > Themes > Apps): a GTK theme, or dark
# (the options have no spaces in them, so they can sit in one variable)
gtk=$(sed -n 's/^gtktheme=//p' /data/ccos.conf 2>/dev/null | head -1 | tr -cd 'A-Za-z0-9._-')
LOOK=""
case "$gtk" in
    "") ;;
    dark) LOOK="--env=GTK_THEME=Adwaita:dark --env=ADW_DEBUG_COLOR_SCHEME=prefer-dark" ;;
    *)  LOOK="--filesystem=$HOME/.themes:ro --env=GTK_THEME=$gtk"
        case "$gtk" in *[Dd]ark*|*[Nn]oir*|*[Nn]ight*) LOOK="$LOOK --env=ADW_DEBUG_COLOR_SCHEME=prefer-dark" ;; esac ;;
esac
exec flatpak run $LOOK "$app" "$@"
