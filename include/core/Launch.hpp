#pragma once
// core::launch -- a device's web page and its ssh command (milestone 15).
// Pure: what to open, not the opening (Shell does that). Not choices either:
// opening a page is not an undo step, like mounting (ARCHITECTURE).
#include "core/Devices.hpp"

#include <string>
#include <vector>

namespace netscope::core::launch {

// Where to reach the device: its advertised host name ("raspi.local"); a
// UPnP-only device (id "upnp-...") by its first address, IPv4 first.
// `bracket` puts an IPv6 address in [] for a URL.
std::string host_of(const Sighting& s, bool bracket);

// The device's web admin: https on 443, else http on 80, else the lowest other
// port, https before http. "" when it serves none. "http://brw90489a9696ae.local/".
std::string web_url(const Sighting& s);

// ssh's arguments for the device ("raspi.local", or "-p", "2222", "nas.local");
// empty when it offers no ssh / sftp-ssh.
std::vector<std::string> ssh_args(const Sighting& s);

// Terminals NetScope knows how to hand a command to, in the order it tries them
// (Fedora's GNOME ships Ptyxis).
const std::vector<std::string>& terminals();

// The argv that opens `terminal` running ssh with `args`. A failed ssh waits
// for Enter, so its error doesn't vanish with the window.
std::vector<std::string> terminal_argv(const std::string& terminal, const std::vector<std::string>& args);

}  // namespace netscope::core::launch
