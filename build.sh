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
cmake --build build --target netscope netscope_selftest netdiscover_selftest netdiscover_probe netdiag_selftest -- -j"$(nproc)"
echo ""
echo "Built: ./build/netscope, ./build/netscope_selftest, ./build/netdiscover_selftest, ./build/netdiscover_probe, ./build/netdiag_selftest"
echo "Smoke: ./build/netscope_selftest && ./build/netdiscover_selftest && ./build/netdiag_selftest"
echo "Network check, no window: ./build/netdiscover_probe 8"
echo "Canned devices instead of Avahi: NETSCOPE_FIXTURE=1 ./build/netscope"
