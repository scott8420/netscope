#pragma once
// netdiscover::Aggregator -- services in, devices out (milestone 4). Pure: no
// D-Bus, no threads, no GTK, so the grouping rules are tested headless.
//
// Avahi speaks in services: "tc01 offers _afpovertcp._tcp on 548 at
// tc01.local, 169.254.192.51". NetScope lists devices (REQUIREMENTS: grouped by
// device, not by service). The Aggregator keeps every resolved service and,
// after each change, says what the affected host now looks like as a
// core::Sighting -- Seen while it has any service left, Lost when the last one
// goes. AvahiSource is the thin D-Bus glue that feeds it.
#include "core/Feed.hpp"

#include <compare>
#include <map>
#include <string>
#include <vector>

namespace netscope::netdiscover {

// One browsed service, as Avahi names it. The same service seen on two
// interfaces or over IPv4 and IPv6 is several keys for one device.
struct ServiceKey {
    int         iface = -1;
    int         proto = -1;
    std::string name;     // instance name: "tc01"
    std::string type;     // "_afpovertcp._tcp"
    std::string domain;   // "local"
    auto operator<=>(const ServiceKey&) const = default;
};

struct Resolved {
    std::string              host;      // "tc01.local"
    std::string              address;   // "169.254.192.51"
    int                      port = 0;
    std::vector<std::string> txt;       // "model=TimeCapsule8,119", ...
    bool operator==(const Resolved&) const = default;
};

// DNS-SD type -> core::Service::kind. "" for a type we don't browse.
std::string kind_for_type(const std::string& type);
// Every type the source browses, in browse order.
const std::vector<std::string>& browsed_types();

class Aggregator {
public:
    // A service resolved (or re-resolved). Events to push, possibly none.
    std::vector<core::FeedEvent> resolved(const ServiceKey& k, const Resolved& r);
    // A service withdrew. Events to push, possibly none.
    std::vector<core::FeedEvent> removed(const ServiceKey& k);

    std::size_t services() const { return m_services.size(); }

private:
    std::vector<core::FeedEvent> report(const core::DeviceId& id);
    core::Sighting build(const core::DeviceId& id) const;

    std::map<ServiceKey, Resolved>             m_services;
    std::map<core::DeviceId, core::Sighting>   m_said;   // last Seen per device
};

// The device id for a host name: lower-case, trailing dot dropped.
core::DeviceId device_id(const std::string& host);

}  // namespace netscope::netdiscover
