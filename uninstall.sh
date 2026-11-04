#!/usr/bin/env bash
# Remove NetScope. Same --system / --prefix as install.sh. Removes exactly the
# paths in the install manifest, and only inside the prefix.
# --purge also removes settings, recents and cache; never documents.
set -euo pipefail

APP=netscope
PREFIX="$HOME/.local"
PURGE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --system) PREFIX=/usr/local ;;
        --prefix) shift; PREFIX="${1:?--prefix needs a directory}" ;;
        --purge)  PURGE=1 ;;
        -h|--help) sed -n '2,4p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

PREFIX="$(realpath -m "$PREFIX")"
MANIFEST="$PREFIX/share/$APP/install-manifest.txt"
if [[ ! -f "$MANIFEST" ]]; then
    echo "no manifest at $MANIFEST: nothing to do" >&2
else
    SUDO=()
    [[ -w "$MANIFEST" ]] || SUDO=(sudo)
    if command -v systemctl >/dev/null && systemctl --user show-environment >/dev/null 2>&1; then
        systemctl --user disable --now netscoped.service >/dev/null 2>&1 || true
    fi
    command -v gnome-extensions >/dev/null && gnome-extensions disable netscope@scott8420.github.io >/dev/null 2>&1 || true
    while IFS= read -r p; do
        [[ -z "$p" ]] && continue
        real="$(realpath -m "$p")"
        case "$real" in
            "$PREFIX"/*) "${SUDO[@]}" rm -f -- "$real" ;;
            *) echo "skipped, outside prefix: $p" >&2 ;;
        esac
    done < "$MANIFEST"
    "${SUDO[@]}" rmdir --ignore-fail-on-non-empty "$PREFIX/share/$APP" 2>/dev/null || true
    EXT="$PREFIX/share/gnome-shell/extensions/netscope@scott8420.github.io"
    "${SUDO[@]}" rmdir --ignore-fail-on-non-empty "$EXT/icons" "$EXT" 2>/dev/null || true
    ICONS="$PREFIX/share/icons/hicolor"
    command -v gtk-update-icon-cache   >/dev/null && [[ -d "$ICONS" ]] && "${SUDO[@]}" gtk-update-icon-cache -q -t -f "$ICONS" || true
    command -v update-desktop-database >/dev/null && "${SUDO[@]}" update-desktop-database -q "$PREFIX/share/applications" || true
    command -v systemctl >/dev/null && systemctl --user daemon-reload >/dev/null 2>&1 || true
    echo "Removed NetScope from $PREFIX"
fi

if [[ $PURGE -eq 1 ]]; then
    rm -rf -- "${XDG_DATA_HOME:-$HOME/.local/share}/$APP" \
              "${XDG_CONFIG_HOME:-$HOME/.config}/$APP" \
              "${XDG_CACHE_HOME:-$HOME/.cache}/$APP"
    echo "Purged settings, recents and cache"
fi
