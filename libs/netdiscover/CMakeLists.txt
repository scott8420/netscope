# libs/netdiscover -- discovery (milestone 4). No GTK: Avahi over D-Bus through
# plain GIO, which gtkmm already brings in, so no new build dependency.
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)

add_library(netdiscover STATIC
    src/Aggregator.cpp
    src/AvahiSource.cpp
    src/Ssdp.cpp            # milestone 13: UPnP / SSDP
    src/Merge.cpp
    src/SsdpSource.cpp
)
target_include_directories(netdiscover PUBLIC include)
target_link_libraries(netdiscover PUBLIC netscope_core PRIVATE PkgConfig::GIO)

# Own tests: the grouping rules, headless, no bus.
add_executable(netdiscover_selftest tests/netdiscover_selftest.cpp)
target_link_libraries(netdiscover_selftest PRIVATE netdiscover)

# A probe for a real network: prints what Avahi reports, no window.
add_executable(netdiscover_probe tests/netdiscover_probe.cpp)
target_link_libraries(netdiscover_probe PRIVATE netdiscover)

# Beside netscope in build/, so every binary is ./build/<name>.
set_target_properties(netdiscover_selftest netdiscover_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
