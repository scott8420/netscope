// netdiscover SSDP, pure parts -- see Ssdp.hpp.
#include "netdiscover/Ssdp.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace netscope::netdiscover::ssdp {

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
std::string decode_entities(std::string s) {
    static const std::pair<const char*, const char*> kE[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
    for (const auto& [from, to] : kE)
        for (std::size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += std::char_traits<char>::length(to))
            s.replace(p, std::char_traits<char>::length(from), to);
    return s;
}
// The text of the first <tag>...</tag> in xml from `from` (namespace prefixes not handled: UPnP uses none).
std::string tag(const std::string& xml, const std::string& name, std::size_t from = 0) {
    const std::string open = "<" + name + ">", close = "</" + name + ">";
    const auto a = xml.find(open, from);
    if (a == std::string::npos) return {};
    const auto b = xml.find(close, a);
    if (b == std::string::npos) return {};
    return decode_entities(trim(xml.substr(a + open.size(), b - a - open.size())));
}
}  // namespace

std::string search_request(int mx) {
    return "M-SEARCH * HTTP/1.1\r\n"
           "HOST: 239.255.255.250:1900\r\n"
           "MAN: \"ssdp:discover\"\r\n"
           "MX: " + std::to_string(mx) + "\r\n"
           "ST: ssdp:all\r\n"
           "USER-AGENT: Linux UPnP/1.1 NetScope/1\r\n\r\n";
}

std::optional<Reply> parse(const std::string& datagram) {
    std::istringstream in(datagram);
    std::string first;
    if (!std::getline(in, first)) return std::nullopt;
    first = trim(first);
    Reply r;
    const std::string f = lower(first);
    const bool answer = f.rfind("http/1.", 0) == 0 && f.find(" 200") != std::string::npos;
    const bool notify = f.rfind("notify * http/1.", 0) == 0;
    if (!answer && !notify) return std::nullopt;
    std::map<std::string, std::string> h;
    for (std::string line; std::getline(in, line);) {
        const auto c = line.find(':');
        if (c == std::string::npos) continue;
        h[lower(trim(line.substr(0, c)))] = trim(line.substr(c + 1));
    }
    r.location = h["location"];
    r.usn = h["usn"];
    r.server = h["server"];
    if (answer) {
        r.kind = Reply::Kind::Answer;
        r.st = h["st"];
    } else {
        const auto nts = lower(h["nts"]);
        if (nts == "ssdp:alive") r.kind = Reply::Kind::Alive;
        else if (nts == "ssdp:byebye") r.kind = Reply::Kind::Byebye;
        else return std::nullopt;   // ssdp:update and friends
        r.st = h["nt"];
    }
    const auto cc = lower(h["cache-control"]);
    if (const auto p = cc.find("max-age"); p != std::string::npos) {
        const auto eq = cc.find('=', p);
        if (eq != std::string::npos) {
            try { r.max_age = std::max(30, std::stoi(cc.substr(eq + 1))); } catch (...) {}
        }
    }
    if (uuid_of(r.usn).empty()) return std::nullopt;
    if (r.kind != Reply::Kind::Byebye && r.location.empty()) return std::nullopt;
    return r;
}

std::string uuid_of(const std::string& usn) {
    const std::string u = lower(trim(usn));
    if (u.rfind("uuid:", 0) != 0) return {};
    const auto end = u.find("::");
    return u.substr(5, end == std::string::npos ? std::string::npos : end - 5);
}

std::optional<Url> parse_url(const std::string& url) {
    const std::string u = trim(url);
    if (lower(u.substr(0, 7)) != "http://") return std::nullopt;
    Url out;
    const auto rest = u.substr(7);
    const auto slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (const auto c = hostport.rfind(':'); c != std::string::npos && hostport.find(']') == std::string::npos) {
        try { out.port = std::stoi(hostport.substr(c + 1)); } catch (...) { return std::nullopt; }
        hostport = hostport.substr(0, c);
    }
    if (hostport.empty() || out.port <= 0 || out.port > 65535) return std::nullopt;
    out.host = hostport;
    return out;
}

Description parse_description(const std::string& xml) {
    Description d;
    const auto dev = xml.find("<device>");
    const std::size_t at = dev == std::string::npos ? 0 : dev;
    d.friendly_name    = tag(xml, "friendlyName", at);
    d.manufacturer     = tag(xml, "manufacturer", at);
    d.model_name       = tag(xml, "modelName", at);
    d.model_number     = tag(xml, "modelNumber", at);
    d.device_type      = tag(xml, "deviceType", at);
    d.presentation_url = tag(xml, "presentationURL", at);
    return d;
}

std::string device_label(const std::string& t) {
    static const std::pair<const char*, const char*> kL[] = {
        {":device:MediaServer:", "Media server"}, {":device:MediaRenderer:", "Media player"},
        {":device:InternetGatewayDevice:", "Router"}, {":device:WANDevice:", "Router"},
        {":device:WLANAccessPointDevice:", "Wi-Fi access point"}, {":device:Printer:", "Printer"},
        {":device:Basic:", "Device"}, {":device:ZonePlayer:", "Speaker"}, {":device:dial:", "TV / streamer"}};
    for (const auto& [k, v] : kL) if (t.find(k) != std::string::npos) return v;
    return {};
}

core::DeviceId device_id(const std::string& uuid) { return "upnp-" + uuid; }

core::Sighting Aggregator::build(const std::string& uuid, const Dev& d) const {
    core::Sighting s;
    s.id = device_id(uuid);
    const auto loc = parse_url(d.location);
    if (loc) s.addresses.push_back(loc->host);
    s.services.push_back({"upnp", loc ? loc->port : 0});
    if (d.desc) {
        s.name = d.desc->friendly_name;
        // The Wi-Fi Alliance (WPS) sub-device of a router calls itself "WFADevice":
        // its model says more.
        if (d.desc->device_type.find("wifialliance") != std::string::npos || s.name == "WFADevice") s.name.clear();
        std::string model = d.desc->model_name;
        if (!d.desc->manufacturer.empty() && model.rfind(d.desc->manufacturer, 0) != 0)
            model = d.desc->manufacturer + (model.empty() ? "" : " " + model);
        if (model.empty()) model = device_label(d.desc->device_type);
        s.model = model;
        // A presentation page is the device's web admin (milestone 15 opens it).
        if (!d.desc->presentation_url.empty()) {
            if (const auto p = parse_url(d.desc->presentation_url)) s.services.push_back({"http", p->port});
            else if (loc) s.services.push_back({"http", loc->port});   // relative URL: same server
        }
    }
    if (s.name.empty()) s.name = !s.model.empty() ? s.model : loc ? loc->host : uuid;   // until the description says
    return s;
}

std::vector<core::FeedEvent> Aggregator::report(const std::string& uuid) {
    const auto it = m_devs.find(uuid);
    const auto said = m_said.find(uuid);
    if (it == m_devs.end()) {
        if (said == m_said.end()) return {};
        m_said.erase(said);
        return {core::FeedEvent{core::FeedEvent::Kind::Lost, core::Sighting{device_id(uuid), {}, {}, {}, {}}, {}}};
    }
    auto s = build(uuid, it->second);
    if (said != m_said.end() && said->second == s) return {};
    m_said[uuid] = s;
    return {core::FeedEvent{core::FeedEvent::Kind::Seen, std::move(s), {}}};
}

std::vector<core::FeedEvent> Aggregator::reply(const Reply& r, Clock::time_point now, bool* fetch) {
    if (fetch) *fetch = false;
    const auto uuid = uuid_of(r.usn);
    if (uuid.empty()) return {};
    if (r.kind == Reply::Kind::Byebye) {
        m_devs.erase(uuid);
        return report(uuid);
    }
    auto& d = m_devs[uuid];
    // Some devices (Scott's ZyXEL router) answer one uuid from two LOCATIONs.
    // Keep the first that gave a description; move only while there is none.
    const bool fresh = d.location.empty() || (d.location != r.location && !d.desc);
    if (fresh) d.location = r.location;
    if (!r.server.empty()) d.server = r.server;
    d.until = std::max(d.until, now + std::chrono::seconds(r.max_age));
    const bool want = fresh || (!d.desc && now - d.tried >= std::chrono::seconds(30));
    if (want) d.tried = now;
    if (fetch) *fetch = want;
    return report(uuid);
}

std::vector<core::FeedEvent> Aggregator::described(const std::string& uuid, const Description& desc) {
    const auto it = m_devs.find(uuid);
    if (it == m_devs.end()) return {};
    it->second.desc = desc;
    return report(uuid);
}

std::vector<core::FeedEvent> Aggregator::expire(Clock::time_point now) {
    std::vector<std::string> gone;
    for (const auto& [u, d] : m_devs) if (d.until <= now) gone.push_back(u);
    std::vector<core::FeedEvent> out;
    for (const auto& u : gone) {
        m_devs.erase(u);
        auto e = report(u);
        out.insert(out.end(), e.begin(), e.end());
    }
    return out;
}

}  // namespace netscope::netdiscover::ssdp
