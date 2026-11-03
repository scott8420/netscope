# libs/netfix -- the Fix button (milestone 9): ipv4.link-local through
# NetworkManager on the system bus. No GTK, no new dependency: GIO's GDBus,
# which giomm already brings in. NetScope never touches the interface itself.
pkg_check_modules(GIO_C REQUIRED IMPORTED_TARGET gio-2.0)

add_library(netfix STATIC src/LinkLocal.cpp)
target_include_directories(netfix PUBLIC include)
target_link_libraries(netfix PUBLIC PkgConfig::GIO_C)

# Own tests: the settings arithmetic on GVariants, no bus.
add_executable(netfix_selftest tests/netfix_selftest.cpp)
target_link_libraries(netfix_selftest PRIVATE netfix)

# A probe against the real NetworkManager: status / on / off.
add_executable(netfix_probe tests/netfix_probe.cpp)
target_link_libraries(netfix_probe PRIVATE netfix)

set_target_properties(netfix_selftest netfix_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
