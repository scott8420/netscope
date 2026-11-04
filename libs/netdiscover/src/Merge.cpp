// netdiscover::Merger -- see Merge.hpp.
#include "netdiscover/Merge.hpp"

#include <algorithm>

namespace netscope::netdiscover {

std::map<core::DeviceId, core::Sighting> Merger::merged() const {
    auto out = m_avahi;
    // Into an Avahi device at the same address first; else into an earlier UPnP
    // device at that address (a router's several uuids -> one row, the lowest id).
    auto at = [&](const core::Sighting& u, bool want_upnp) -> core::Sighting* {
        for (auto& [hid, h] : out) {
            if ((hid.rfind("upnp-", 0) == 0) != want_upnp) continue;
            for (const auto& a : u.addresses)
                if (std::find(h.addresses.begin(), h.addresses.end(), a) != h.addresses.end()) return &h;
        }
        return nullptr;
    };
    for (const auto& [id, u] : m_ssdp) {
        core::Sighting* host = at(u, false);
        if (!host) host = at(u, true);
        if (!host) { out[id] = u; continue; }
        for (const auto& svc : u.services)
            if (std::find(host->services.begin(), host->services.end(), svc) == host->services.end())
                host->services.push_back(svc);
        std::sort(host->services.begin(), host->services.end(), [](const core::Service& a, const core::Service& b) {
            return a.kind != b.kind ? a.kind < b.kind : a.port < b.port;
        });
        if (host->model.empty()) host->model = u.model;
    }
    return out;
}

std::vector<core::FeedEvent> Merger::take(From from, const core::FeedEvent& e) {
    if (e.kind == core::FeedEvent::Kind::Note) return {e};
    auto& side = from == From::Avahi ? m_avahi : m_ssdp;
    if (e.kind == core::FeedEvent::Kind::Seen) side[e.sighting.id] = e.sighting;
    else side.erase(e.sighting.id);

    const auto now = merged();
    std::vector<core::FeedEvent> out;
    for (const auto& [id, s] : m_said)
        if (!now.count(id)) {
            // Still announced but folded into a host: Merged (off the list); else Lost (stale).
            const bool folded = m_ssdp.count(id) > 0;
            out.push_back({folded ? core::FeedEvent::Kind::Merged : core::FeedEvent::Kind::Lost,
                           core::Sighting{id, {}, {}, {}, {}}, {}});
        }
    for (const auto& [id, s] : now) {
        const auto was = m_said.find(id);
        if (was == m_said.end() || !(was->second == s)) out.push_back({core::FeedEvent::Kind::Seen, s, {}});
    }
    m_said = now;
    return out;
}

}  // namespace netscope::netdiscover
