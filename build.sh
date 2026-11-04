#!/usr/bin/env bash
# NetScope build.
# Deps (Fedora): gtkmm4.0-devel spdlog-devel cmake gcc-c++ glib2-devel libsecret-devel
# Deps (Ubuntu): libgtkmm-4.0-dev libspdlog-dev cmake g++ libsecret-1-dev
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"
GEN=()
if [[ ! -f build/CMakeCache.txt ]] && command -v ninja >/dev/null; then
    GEN=(-G Ninja)
fi
cmake -B build "${GEN[@]}"
cmake --build build --target netscope netscope_selftest netdiscover_selftest netdiscover_probe netdiag_selftest netmount_selftest netmount_probe netfix_selftest netfix_probe netafp_selftest netafp_probe netscoped -- -j"$(nproc)"
echo ""
echo "Built: ./build/netscope, ./build/netscope_selftest, ./build/netdiscover_selftest, ./build/netdiscover_probe, ./build/netdiag_selftest, ./build/netmount_selftest, ./build/netmount_probe, ./build/netfix_selftest, ./build/netfix_probe, ./build/netafp_selftest, ./build/netafp_probe, ./build/netscoped"
echo "Smoke: ./build/netscope_selftest && ./build/netdiscover_selftest && ./build/netdiag_selftest && ./build/netmount_selftest && ./build/netfix_selftest && ./build/netafp_selftest"
echo "Network check, no window: ./build/netdiscover_probe 8"
echo "Shares, no window: ./build/netmount_probe list smb://raspi.local/   (NFS exports: ./build/netmount_probe exports <address>)"
echo "Fix (169.254 reachable), no window: ./build/netfix_probe status   (then: on / off)"
echo "AFP, no window: ./build/netafp_probe volumes 169.254.192.51 scott   (then: mount <address> Data scott)"
echo "Tile (needs gnome-shell): tests/gui_tile_shell.sh [outdir]"
echo "Canned devices instead of Avahi: NETSCOPE_FIXTURE=1 ./build/netscope"
