# libs/netdiag -- the diagnostic checks (milestone 5). No GTK, no new deps.
add_library(netdiag STATIC src/Checks.cpp)
target_include_directories(netdiag PUBLIC include)
target_link_libraries(netdiag PUBLIC netscope_core)

add_executable(netdiag_selftest tests/netdiag_selftest.cpp)
target_link_libraries(netdiag_selftest PRIVATE netdiag)
set_target_properties(netdiag_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
