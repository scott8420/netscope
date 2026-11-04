# libs/netafp -- AFP through Netatalk Client (milestone 10). No GTK.
# Netatalk Client installs to /usr/local by default, which pkg-config doesn't
# search on Fedora: look there too. Optional: without libafpsl NetScope builds
# and says AFP is off.
set(_afp_pc "/usr/local/lib64/pkgconfig:/usr/local/lib/pkgconfig:/usr/local/share/pkgconfig")
file(GLOB _afp_multiarch "/usr/local/lib/*-linux-gnu/pkgconfig")
foreach(d ${_afp_multiarch})
    string(APPEND _afp_pc ":${d}")
endforeach()
set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:${_afp_pc}")
pkg_check_modules(AFPSL QUIET IMPORTED_TARGET libafpsl libafpclient)

add_library(netafp STATIC src/Afp.cpp src/Keyring.cpp)
target_include_directories(netafp PUBLIC include)

# Milestone 12: AFP passwords in GNOME Keyring. Optional: without libsecret the
# app keeps the password until it quits. (Fedora: libsecret-devel.)
pkg_check_modules(LIBSECRET QUIET IMPORTED_TARGET libsecret-1)
if(LIBSECRET_FOUND)
    message(STATUS "netafp: libsecret ${LIBSECRET_VERSION} found -- AFP passwords in Keyring")
    target_compile_definitions(netafp PRIVATE NETAFP_HAVE_LIBSECRET=1)
    target_link_libraries(netafp PRIVATE PkgConfig::LIBSECRET)
else()
    message(WARNING "netafp: libsecret not found -- AFP passwords won't go to Keyring "
                    "(the tile will open the app for them). Fedora: sudo dnf install libsecret-devel")
endif()
if(AFPSL_FOUND)
    message(STATUS "netafp: Netatalk Client found (libafpsl ${AFPSL_libafpsl_VERSION}) -- AFP on")
    target_compile_definitions(netafp PRIVATE NETAFP_HAVE_AFPSL=1)
    target_link_libraries(netafp PRIVATE PkgConfig::AFPSL)
else()
    message(WARNING "netafp: Netatalk Client (libafpsl) not found -- AFP will be off. "
                    "Build and install netatalk-client 0.9.5, then re-run cmake.")
endif()

# Own tests: URIs, mount points, mountinfo; headless, no server.
add_executable(netafp_selftest tests/netafp_selftest.cpp)
target_link_libraries(netafp_selftest PRIVATE netafp)

# A probe for a real server: volumes / mount / unmount with no window.
add_executable(netafp_probe tests/netafp_probe.cpp)
target_link_libraries(netafp_probe PRIVATE netafp)

set_target_properties(netafp_selftest netafp_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
