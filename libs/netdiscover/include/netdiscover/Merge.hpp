#pragma once
// netdiscover::Merger -- several discovery sources, one device list
// (milestone 13). Pure.
//
// Avahi names devices by host ("raspi.local"); SSDP by uuid ("upnp-2fac...").
// The same box often speaks both (RASPI with a DLNA server, a NAS, a router).
// Rule: a UPnP device whose address is one of an Avahi device's addresses is
// folded INTO that device -- its services added, its model used when Avahi had
// none -- and doesn't appear on its own. Otherwise it is listed under its own
// id. Avahi's id wins, so nicknames and pins (keyed on the id) stay put.
//
// Each input replaces that source's view of one device; the Merger recomputes
// the merged list and says what changed (Seen / Lost), so a fold is a Merged
// for the UPnP id (off the list, not stale) plus a Seen for the host.
#include "core/Feed.hpp"

#include <map>
#include <string>
#include <vector>

namespace netscope::netdiscover {

class Merger {
public:
    enum class From { Avahi, Ssdp };
    std::vector<core::FeedEvent> take(From from, const core::FeedEvent& e);   // Notes pass through

private:
    std::map<core::DeviceId, core::Sighting> merged() const;
    std::map<core::DeviceId, core::Sighting> m_avahi, m_ssdp, m_said;
};

}  // namespace netscope::netdiscover
