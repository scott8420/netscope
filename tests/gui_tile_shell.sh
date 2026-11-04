#!/usr/bin/env bash
# gui_tile_shell.sh -- the NetScope tile inside a REAL (headless) GNOME Shell
# (milestone 11; Samson's s011 harness). Mock netscoped + mock app on a
# private session bus (tests/tile_shell_mock.py); a helper extension turns on
# the Shell's unsafe mode so this script can drive it over
# org.gnome.Shell.Eval and take screenshots. Sandbox: Ubuntu 24.04's GNOME
# Shell 46 -- Scott runs 50, so his eyes decide "done".
#   tests/gui_tile_shell.sh [outdir]    needs: gnome-shell, python3-dbus, gdbus
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$here/.."
outdir="${1:-$(mktemp -d)}"; mkdir -p "$outdir"
command -v gnome-shell >/dev/null || { echo "SKIP: no gnome-shell"; exit 0; }
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
export HOME="$T" XDG_DATA_HOME="$T/share" XDG_CONFIG_HOME="$T/config" XDG_CACHE_HOME="$T/cache" XDG_RUNTIME_DIR="$T/run"
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
uuid=netscope@scott8420.github.io
ext="$XDG_DATA_HOME/gnome-shell/extensions"
mkdir -p "$ext/$uuid/icons" "$ext/unsafe@netscope.test"
cp "$root/shell-extension/$uuid/"* "$ext/$uuid/" 2>/dev/null
cp "$root/resources/icons/scalable/apps/netscope-logo-symbolic.svg" "$ext/$uuid/icons/"
cat > "$ext/unsafe@netscope.test/metadata.json" <<'J'
{"uuid": "unsafe@netscope.test", "name": "test: unsafe mode", "description": "tests only", "shell-version": ["46","47","48","49","50"]}
J
cat > "$ext/unsafe@netscope.test/extension.js" <<'J'
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
export default class { enable() { global.context.unsafe_mode = true; globalThis.NetScopeTestMain = Main; } disable() {} }
J
# GNOME Shell wants a system bus too (logind, accounts): an empty private one.
cat > "$T/sys.conf" <<B
<busconfig><type>system</type><listen>unix:path=$T/sys.sock</listen>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/><allow own="*"/><allow user="*"/></policy>
<auth>EXTERNAL</auth></busconfig>
B
dbus-daemon --config-file="$T/sys.conf" --fork --print-pid > "$T/sys.pid"
for _ in $(seq 30); do [[ -S $T/sys.sock ]] && break; sleep 0.1; done; sleep 0.3
export DBUS_SYSTEM_BUS_ADDRESS="unix:path=$T/sys.sock"
trap 'kill "$(cat "$T/sys.pid")" 2>/dev/null; rm -rf "$T"' EXIT
py=""; for p in python3 python3.12 python3.11 /usr/bin/python3.12 /usr/bin/python3; do command -v $p >/dev/null && $p -c "import dbus, dbus.mainloop.glib; from gi.repository import GLib" 2>/dev/null && py=$p && break; done
[[ -n $py ]] || { echo "SKIP: no python3 with dbus + gi"; exit 0; }
export outdir uuid here py
dbus-run-session -- bash -c '
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  PASS  $1"; }
bad() { fail=$((fail+1)); echo "  FAIL  $1"; }
log="$outdir/calls.log"; : > "$log"
$py "$here/tile_shell_mock.py" "$log" & mock=$!
for _ in $(seq 50); do grep -q ready "$log" && break; sleep 0.1; done
gsettings set org.gnome.shell disable-user-extensions false
gsettings set org.gnome.shell enabled-extensions "[\"unsafe@netscope.test\", \"$uuid\"]"
gsettings set org.gnome.desktop.interface color-scheme prefer-dark
gnome-shell --headless --wayland --no-x11 --virtual-monitor 1280x800 > "$outdir/shell.log" 2>&1 & shell=$!
for _ in $(seq 150); do gdbus call --session -d org.gnome.Shell -o /org/gnome/Shell -m org.gnome.Shell.Eval "1" 2>/dev/null | grep -q "(true" && break; sleep 0.2; done
E() { gdbus call --session -d org.gnome.Shell -o /org/gnome/Shell -m org.gnome.Shell.Eval "$1" 2>&1; }
shot() { gdbus call --session -d org.gnome.Shell -o /org/gnome/Shell/Screenshot -m org.gnome.Shell.Screenshot.Screenshot false false "$outdir/$1.png" >/dev/null 2>&1; }
info=$(gdbus call --session -d org.gnome.Shell -o /org/gnome/Shell -m org.gnome.Shell.Extensions.GetExtensionInfo "$uuid" 2>&1)
echo "$info" | grep -q "'"'"'state'"'"': <1.0>" && ok "the Shell loaded the extension: state ENABLED" || { bad "extension state"; echo "$info" | tr , "\n" | grep -E "state|error"; }
echo "$info" | grep -q "'"'"'error'"'"': <'"'"''"'"'>" && ok "no load error" || bad "load error"
E "1" | grep -q "(true" && ok "unsafe mode on (Eval works)" || bad "Eval not allowed"
Q="const qs = NetScopeTestMain.panel.statusArea.quickSettings; const t = qs.menu._grid.get_children().find(c => c.title === \"NetScope\");"
r=$(E "$Q t ? [t.subtitle, t.checked].join(\"|\") : \"none\"")
echo "$r" | grep -q "pub mounted|true" && ok "tile: subtitle \"pub mounted\", switch lit" || bad "tile subtitle/checked: $r"
E "$Q qs.menu.open(); true" >/dev/null; sleep 1.2; shot 01-quick-settings
E "$Q t.menu.open(); true" >/dev/null; sleep 1.2; shot 02-tile-menu
r=$(E "$Q t._shareSection.box.get_children().length + \"|\" + t._unmountAllItem.sensitive")
echo "$r" | grep -q "2|true" && ok "menu: 2 pinned shares, Unmount All live" || bad "menu content: $r"
grep -q Refresh "$log" && ok "opening the menu asks netscoped to Refresh" || bad "Refresh on menu open"
E "$Q t._shareSection.box.get_children()[1].emit(\"activate\", null); true" >/dev/null; sleep 0.8
grep -q "^Unmount smb://raspi.local/pub/" "$log" && ok "the mounted row (pub) -> Unmount on netscoped" || bad "row Unmount"
r=$(E "$Q [t.subtitle, t.checked].join(\"|\")")
echo "$r" | grep -q "Nothing mounted|false" && ok "after PropertiesChanged: \"Nothing mounted\", switch off" || bad "after unmount: $r"
E "$Q t.emit(\"clicked\", null); true" >/dev/null; sleep 1
grep -q "^Mount afp://tc01.local/Data/" "$log" && ok "switch on -> Mount the first pinned share (Data)" || bad "switch on -> Mount"
grep -q "^ActivateAction mount afp://tc01.local/Data/" "$log" && ok "\"password\" -> the app asks (app.mount Data)" || { bad "password -> app.mount"; cat "$log"; }
grep -q "^Mount smb://raspi.local/pub/" "$log" && bad "kept mounting after a password was needed" || ok "stops at the password: one dialog at a time"
E "$Q t.menu.open(); true" >/dev/null; sleep 0.5
E "$Q t._shareSection.box.get_children()[1].emit(\"activate\", null); true" >/dev/null; sleep 0.8
grep -q "^Mount smb://raspi.local/pub/" "$log" && ok "an unmounted row (pub) -> Mount on netscoped" || bad "row Mount"
r=$(E "$Q [t.subtitle, t.checked].join(\"|\")")
echo "$r" | grep -q "pub mounted|true" && ok "mounted again: switch lit" || bad "after mount: $r"
E "$Q qs.menu.open(); t.menu.open(); true" >/dev/null; sleep 1.2; shot 03-tile-menu-mounted
E "$Q t.emit(\"clicked\", null); true" >/dev/null; sleep 0.8
grep -q "^UnmountAll" "$log" && ok "switch off -> UnmountAll" || bad "switch off -> UnmountAll"
# Milestone 12: the password for Data in Keyring -> netscoped mounts it, slowly; the row says so.
gdbus call --session -d io.github.scott8420.NetScope.Daemon -o /io/github/scott8420/NetScope/Daemon -m io.github.scott8420.NetScope.Daemon1.TestKeyring >/dev/null
asks=$(grep -c "^ActivateAction mount" "$log")
E "$Q t.menu.open(); t._shareSection.box.get_children()[0]._act(); true" >/dev/null; sleep 1
r=$(E "$Q [t.subtitle, (function w(a) { return [a.text].concat(...a.get_children().map(w)); })(t._shareSection.box.get_children()[0]).filter(x => typeof x === \"string\").join(\"/\")].join(\"|\")")
echo "$r" | grep -qE "Mounting Data.?\|.*mounting.? . on tc01 . AFP" && ok "Keyring mount under way: \"Mounting Data…\", row \"mounting…\"" || bad "busy state: $r"
E "$Q qs.menu.open(); t.menu.open(); true" >/dev/null; sleep 0.8; shot 05-mounting
sleep 3
r=$(E "$Q [t.subtitle, t.checked].join(\"|\")")
echo "$r" | grep -q "Data mounted|true" && ok "after 3 s: \"Data mounted\", switch lit" || bad "after keyring mount: $r"
[[ $(grep -c "^ActivateAction mount" "$log") -eq $asks ]] && ok "no app window for a Keyring mount" || bad "the app was asked"
E "$Q t.emit(\"clicked\", null); true" >/dev/null; sleep 0.8
E "$Q t.menu.open(); const it = t.menu._getMenuItems().find(i => i.label && i.label.text === \"Open NetScope\"); it.activate(null); true" >/dev/null; sleep 0.5
grep -q "^ActivateAction devices" "$log" && ok "Open NetScope -> app.devices" || bad "Open NetScope action"
kill $mock; sleep 1.5
r=$(E "$Q [t.subtitle, t.checked, t._statusItem.label.text].join(\"|\")")
echo "$r" | grep -q "Service off|false|.*netscoped is not running" && ok "netscoped stopped -> \"Service off\" and a plain line" || bad "service off: $r"
E "$Q qs.menu.open(); t.menu.open(); true" >/dev/null; sleep 1.2; shot 04-service-off
grep -iE "netscope.*(error|exception)|JS ERROR" "$outdir/shell.log" | head -5
grep -qiE "JS ERROR|netscope@.*Error" "$outdir/shell.log" && bad "JS errors in the Shell log" || ok "no JS errors in the Shell log"
kill $shell 2>/dev/null; wait $shell 2>/dev/null
echo "-----------------------------------------------"
echo "$pass pass / $fail fail"
[[ $fail -eq 0 ]]
'
