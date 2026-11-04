#!/usr/bin/env bash
# Install NetScope. Default ~/.local (no sudo); --system = /usr/local;
# --prefix DIR; --build rebuilds first. sudo only if the prefix isn't writable.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

APP=netscope
ID=io.github.scott8420.NetScope
PREFIX="$HOME/.local"
BUILD=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --system) PREFIX=/usr/local ;;
        --prefix) shift; PREFIX="${1:?--prefix needs a directory}" ;;
        --build)  BUILD=1 ;;
        -h|--help) sed -n '2,3p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

[[ $BUILD -eq 1 ]] && ./build.sh
if [[ ! -x build/$APP ]]; then
    echo "build/$APP not found: run ./build.sh or pass --build" >&2
    exit 1
fi

# Writable, or the nearest existing parent is.
probe="$PREFIX"
while [[ ! -e "$probe" ]]; do probe="$(dirname "$probe")"; done
SUDO=()
[[ -w "$probe" ]] || SUDO=(sudo)

BIN="$PREFIX/bin"
APPS="$PREFIX/share/applications"
ICONS="$PREFIX/share/icons/hicolor"
SHARE="$PREFIX/share/$APP"
MANIFEST="$SHARE/install-manifest.txt"

DBUS="$PREFIX/share/dbus-1/services"
UNITS="$PREFIX/share/systemd/user"   # systemd's user search path covers ~/.local/share and /usr/local/share

tmp="$(mktemp)"; gen="$(mktemp -d)"
trap 'rm -rf "$tmp" "$gen"' EXIT
fill() { sed "s|@BINDIR@|$BIN|g" "data/$1.in" > "$gen/$1"; echo "$gen/$1"; }

put() {  # mode source dest
    "${SUDO[@]}" install -D -m "$1" "$2" "$3"
    echo "$3" >> "$tmp"
}
put 755 "build/$APP"                                                "$BIN/$APP"
put 755 "build/netscoped"                                           "$BIN/netscoped"
put 644 "$(fill "$ID.desktop")"                                     "$APPS/$ID.desktop"
# Milestone 8: the tile starts the app (D-Bus activation, app.mount) and the
# daemon (D-Bus activation hands over to the systemd user unit).
put 644 "$(fill "$ID.service")"                                     "$DBUS/$ID.service"
put 644 "$(fill "$ID.Daemon.service")"                              "$DBUS/$ID.Daemon.service"
put 644 "$(fill netscoped.service)"                                 "$UNITS/netscoped.service"
put 644 "data/$ID.svg"                                              "$ICONS/scalable/apps/$ID.svg"
put 644 "resources/icons/scalable/apps/$APP-logo-symbolic.svg"      "$ICONS/symbolic/apps/$ID-symbolic.svg"
# Milestone 11: the Quick Settings tile (a GNOME Shell extension). GNOME looks
# in ~/.local/share and /usr/local/share alike.
UUID=netscope@scott8420.github.io
EXT="$PREFIX/share/gnome-shell/extensions/$UUID"
for f in metadata.json extension.js stylesheet.css; do
    put 644 "shell-extension/$UUID/$f"                              "$EXT/$f"
done
put 644 "resources/icons/scalable/apps/$APP-logo-symbolic.svg"      "$EXT/icons/$APP-logo-symbolic.svg"

echo "$MANIFEST" >> "$tmp"
"${SUDO[@]}" install -D -m 644 "$tmp" "$MANIFEST"

command -v gtk-update-icon-cache   >/dev/null && "${SUDO[@]}" gtk-update-icon-cache -q -t -f "$ICONS" || true
command -v update-desktop-database >/dev/null && "${SUDO[@]}" update-desktop-database -q "$APPS" || true

if [[ -z ${SUDO[*]:-} ]] && command -v systemctl >/dev/null && systemctl --user show-environment >/dev/null 2>&1; then
    systemctl --user daemon-reload
    systemctl --user enable netscoped.service >/dev/null 2>&1 || true
    systemctl --user restart netscoped.service && echo "netscoped: running (systemctl --user status netscoped)"
else
    echo "netscoped: start it with  systemctl --user daemon-reload && systemctl --user enable --now netscoped"
fi
# Switch the tile on. A NEW extension is found only at login (Samson s011).
new_ext=1
command -v gnome-extensions >/dev/null && gnome-extensions info "$UUID" >/dev/null 2>&1 && new_ext=0
if command -v gnome-extensions >/dev/null && gnome-extensions enable "$UUID" >/dev/null 2>&1; then
    echo "tile: enabled"
elif command -v gsettings >/dev/null && gsettings get org.gnome.shell enabled-extensions >/dev/null 2>&1; then
    cur=$(gsettings get org.gnome.shell enabled-extensions)
    if [[ $cur != *"'$UUID'"* ]]; then
        if [[ $cur == "@as []" || $cur == "[]" ]]; then new="['$UUID']"; else new="${cur%]}, '$UUID']"; fi
        gsettings set org.gnome.shell enabled-extensions "$new"
    fi
    echo "tile: switched on for your next login"
else
    echo "tile: installed (turn it on in Extensions)"
fi
if [[ $new_ext == 1 ]]; then
    echo "  -> GNOME finds a NEW extension only at login: log out and back in."
else
    echo "  -> a changed tile shows after you log out and back in."
fi
echo "Installed to $PREFIX"
