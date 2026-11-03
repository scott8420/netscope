#include "netdiscover/Aggregator.hpp"

#include <algorithm>
#include <cctype>

namespace netscope::netdiscover {

namespace {
struct TypeKind { const char* type; const char* kind; };
// Browse order is display-neutral; rows() orders what the user sees.
constexpr TypeKind kTypes[] = {
    {"_afpovertcp._tcp", "afp"},
    {"_smb._tcp",        "smb"},
    {"_nfs._tcp",        "nfs"},
    {"_sftp-ssh._tcp",   "sftp"},
    {"_ssh._tcp",        "ssh"},
    {"_http._tcp",       "http"},
    {"_https._tcp",      "https"},
    {"_device-info._tcp","device-info"},   // model= in TXT; no connection
    {"_adisk._tcp",      "adisk"},         // Time Machine disk; bookkeeping
};

bool is_v4(const std::string& a) { return a.find(':') == std::string::npos; }
// This machine's own services resolve to loopback too; that's no use to reach
// it from anywhere else, so it goes last.
bool is_loopback(const std::string& a) { return a.rfind("127.", 0) == 0 || a == "::1"; }
}  // namespace

std::string kind_for_type(const std::string& type) {
    for (const auto& t : kTypes) if (type == t.type) return t.kind;
    return {};
}

const std::vector<std::string>& browsed_types() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> o;
        for (const auto& t : kTypes) o.emplace_back(t.type);
        return o;
    }();
    return v;
}

core::DeviceId device_id(const std::string& host) {
    std::string s = host;
    while (!s.empty() && s.back() == '.') s.pop_back();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

core::Sighting Aggregator::build(const core::DeviceId& id) const {
    core::Sighting s;
    s.id = id;
    std::vector<std::string> v4, v6;
    for (const auto& [k, r] : m_services) {
        if (device_id(r.host) != id) continue;
        if (s.name.empty()) {
            const auto dot = r.host.find('.');
            s.name = r.host.substr(0, dot);
        }
        if (!r.address.empty()) {
            auto& bucket = is_v4(r.address) ? v4 : v6;
            if (std::find(bucket.begin(), bucket.end(), r.address) == bucket.end())
                bucket.push_back(r.address);
        }
        const core::Service svc{kind_for_type(k.type), r.port};
        // device-info and adisk are announcements, not things to connect to.
        if (!svc.kind.empty() && svc.kind != "device-info" && svc.kind != "adisk" &&
            std::find(s.services.begin(), s.services.end(), svc) == s.services.end())
            s.services.push_back(svc);
        if (k.type == "_device-info._tcp")
            for (const auto& t : r.txt)
                if (t.rfind("model=", 0) == 0) s.model = t.substr(6);
    }
    const auto order = [](const std::string& a, const std::string& b) {
        if (is_loopback(a) != is_loopback(b)) return !is_loopback(a);
        return a < b;
    };
    std::sort(v4.begin(), v4.end(), order);
    std::sort(v6.begin(), v6.end(), order);
    s.addresses = v4;
    s.addresses.insert(s.addresses.end(), v6.begin(), v6.end());
    std::sort(s.services.begin(), s.services.end(), [](const core::Service& a, const core::Service& b) {
        return a.kind != b.kind ? a.kind < b.kind : a.port < b.port;
    });
    return s;
}

std::vector<core::FeedEvent> Aggregator::report(const core::DeviceId& id) {
    const bool any = std::any_of(m_services.begin(), m_services.end(),
                                 [&](const auto& kv) { return device_id(kv.second.host) == id; });
    const auto said = m_said.find(id);
    if (!any) {
        if (said == m_said.end()) return {};
        m_said.erase(said);
        return {core::FeedEvent{core::FeedEvent::Kind::Lost, core::Sighting{id, {}, {}, {}, {}}, {}}};
    }
    auto s = build(id);
    if (said != m_said.end() && said->second == s) return {};   // nothing the list shows changed
    m_said[id] = s;
    return {core::FeedEvent{core::FeedEvent::Kind::Seen, std::move(s), {}}};
}

std::vector<core::FeedEvent> Aggregator::resolved(const ServiceKey& k, const Resolved& r) {
    if (r.host.empty()) return {};
    std::vector<core::FeedEvent> out;
    const auto it = m_services.find(k);
    if (it != m_services.end()) {
        if (it->second == r) return {};
        // The service moved to another host: the old host may have lost it.
        const auto old = device_id(it->second.host);
        it->second = r;
        if (old != device_id(r.host)) out = report(old);
    } else {
        m_services.emplace(k, r);
    }
    auto more = report(device_id(r.host));
    out.insert(out.end(), more.begin(), more.end());
    return out;
}

std::vector<core::FeedEvent> Aggregator::removed(const ServiceKey& k) {
    const auto it = m_services.find(k);
    if (it == m_services.end()) return {};
    const auto id = device_id(it->second.host);
    m_services.erase(it);
    return report(id);
}

}  // namespace netscope::netdiscover
