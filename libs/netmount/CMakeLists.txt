# libs/netmount -- shares and mounts through GIO (milestone 6). No GTK: giomm,
# which gtkmm already brings in, so no new build dependency. Passwords go to
# GNOME Keyring through gvfs, not through code of ours (see Mounter.hpp).
pkg_check_modules(GIOMM REQUIRED IMPORTED_TARGET giomm-2.68)

add_library(netmount STATIC src/Places.cpp src/Mounter.cpp)
target_include_directories(netmount PUBLIC include)
target_link_libraries(netmount PUBLIC netscope_core PkgConfig::GIOMM)

# Own tests: the URI rules, headless, no bus.
add_executable(netmount_selftest tests/netmount_selftest.cpp)
target_link_libraries(netmount_selftest PRIVATE netmount)

# A probe for a real server: list / mount / unmount with no window.
add_executable(netmount_probe tests/netmount_probe.cpp)
target_link_libraries(netmount_probe PRIVATE netmount)

set_target_properties(netmount_selftest netmount_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
