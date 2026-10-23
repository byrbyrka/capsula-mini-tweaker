#!/bin/sh
set -eu
[ "$(id -u)" = 0 ] || { echo 'Run as root ON THE CAPSULE.' >&2; exit 1; }
[ -f /etc/alpine-release ] || { echo 'This installer is for Alpine Linux on the capsule.' >&2; exit 1; }
[ "$(uname -m)" = aarch64 ] || { echo 'This build is for ARM64 / Amlogic A113X.' >&2; exit 1; }
command -v ringled >/dev/null || { echo 'Firmware ringled utility is missing.' >&2; exit 1; }
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BIN="$HERE/bin/capsule-tweaker-aarch64"
[ -s "$BIN" ] || { echo 'Native binary is missing from bin/.' >&2; exit 1; }

# Reuse the firmware libraries. No Python, compiler, or build dependencies.
[ -s /etc/ssl/cert.pem ] || apk add --no-cache ca-certificates
[ -f /usr/share/zoneinfo/Europe/Moscow ] || apk add --no-cache tzdata
chmod +x "$BIN"
"$BIN" --version
"$BIN" --preflight || { echo 'Hardware check failed: verify ALSA Master/Playback, buttons and display.' >&2; exit 1; }
FIRST=first
if [ -f /etc/init.d/capsule-controls ]; then
    FIRST=upgrade
    rc-service capsule-controls stop || true
    if rc-service capsule-controls status >/dev/null 2>&1; then
        echo 'Old service is still running; installation stopped.' >&2
        exit 1
    fi
fi
install -d -m 0755 /usr/local/bin /var/lib/capsule-controls
install -m 0755 "$BIN" /usr/local/bin/.capsule-tweaker.new
mv -f /usr/local/bin/.capsule-tweaker.new /usr/local/bin/capsule-tweaker
install -m 0755 "$HERE/capsule-controls.init" /etc/init.d/capsule-controls
capsule-tweaker --prepare "$FIRST"
# Remove only this tweaker's obsolete installed Python runtime.
if [ -f /usr/local/lib/capsule-controls/capsule_controls.py ]; then
    rm -rf /usr/local/lib/capsule-controls
fi
rc-update add capsule-controls default
rc-service capsule-controls start
sync
printf '%s\n' 'Installed native C version. Open the menu: capsule-tweaker'
