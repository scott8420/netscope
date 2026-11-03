#!/usr/bin/env bash
# NetScope build.
# Deps (Fedora): gtkmm4.0-devel spdlog-devel cmake gcc-c++ glib2-devel
# Deps (Ubuntu): libgtkmm-4.0-dev libspdlog-dev cmake g++
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"
GEN=()
if [[ ! -f build/CMakeCache.txt ]] && command -v ninja >/dev/null; then
    GEN=(-G Ninja)
fi
cmake -B build "${GEN[@]}"
cmake --build build --target netscope netscope_selftest netdiscover_selftest netdiscover_probe netdiag_selftest netmount_selftest netmount_probe netfix_selftest netfix_probe netscoped -- -j"$(nproc)"
echo ""
echo "Built: ./build/netscope, ./build/netscope_selftest, ./build/netdiscover_selftest, ./build/netdiscover_probe, ./build/netdiag_selftest, ./build/netmount_selftest, ./build/netmount_probe, ./build/netfix_selftest, ./build/netfix_probe, ./build/netscoped"
echo "Smoke: ./build/netscope_selftest && ./build/netdiscover_selftest && ./build/netdiag_selftest && ./build/netmount_selftest && ./build/netfix_selftest"
echo "Network check, no window: ./build/netdiscover_probe 8"
echo "Shares, no window: ./build/netmount_probe list smb://raspi.local/"
echo "Fix (169.254 reachable), no window: ./build/netfix_probe status   (then: on / off)"
echo "Canned devices instead of Avahi: NETSCOPE_FIXTURE=1 ./build/netscope"
