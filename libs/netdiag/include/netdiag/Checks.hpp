#pragma once
// netdiag -- why can't I reach it? (milestone 5). No GTK.
//
// Each check answers in a plain-English sentence (REQUIREMENTS: Diagnose).
// The checks that only reason -- what kind of address is this, does this
// machine have a way to it -- are pure functions over Facts, tested headless.
// Only local_facts() (reads this machine) and check_port() (opens a socket)
// touch the system, and check_port blocks: callers run it off the main thread.
//
// Protocol checks (AFP / SMB handshakes) are not here yet: they come with the
// mounts they serve (milestones 6 and 7).
#include "core/Devices.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace netscope::netdiag {

enum class Verdict { Ok, Warn, Fail, Skip };

struct Finding {
    std::string check;     // "address", "route", "port 548"
    Verdict     verdict = Verdict::Skip;
    std::string says;      // one plain sentence
    bool operator==(const Finding&) const = default;
};

// This machine, as far as reaching others goes.
struct Iface {
    std::string name;      // "wld0"
    std::string addr;      // IPv4 dotted
    int         prefix = 0;
};
struct Facts {
    std::vector<Iface> ifaces;          // up, IPv4, not loopback
    bool               default_route = false;
};
Facts local_facts();                    // getifaddrs + /proc/net/route

enum class Kind { None, Loopback, LinkLocal, Private, Public, V6 };
Kind kind_of(const std::string& addr);

// Pure.
Finding check_address(const core::Sighting& s);
Finding check_route(const core::Sighting& s, const Facts& f);
// Blocking, bounded by `timeout`.
Finding check_port(const std::string& addr, int port, const std::string& service,
                   std::chrono::milliseconds timeout);

// All of them for one device: address, route, then each service's port (on
// the first IPv4 address). Blocking.
std::vector<Finding> diagnose(const core::Sighting& s, const Facts& f,
                              std::chrono::milliseconds timeout = std::chrono::seconds(3));

}  // namespace netscope::netdiag
