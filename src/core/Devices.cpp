#include "core/Devices.hpp"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace netscope::core {

// ── Network ────────────────────────────────────────────────────────────────

void Network::seen(const Sighting& s) {
    if (s.id.empty()) return;
    Seen v{s, Presence::Online};
    const auto it = m_items.find(s.id);
    if (it != m_items.end() && it->second == v) return;   // a repeat announcement: no repaint
    m_items[s.id] = std::move(v);
    if (m_changed) m_changed();
}

bool Network::forget(const DeviceId& id) {
    if (m_items.erase(id) == 0) return false;
    if (m_changed) m_changed();
    return true;
}

bool Network::lost(const DeviceId& id) {
    const auto it = m_items.find(id);
    if (it == m_items.end()) return false;
    if (it->second.presence == Presence::Stale) return true;
    it->second.presence = Presence::Stale;
    if (m_changed) m_changed();
    return true;
}

std::optional<Seen> Network::get(const DeviceId& id) const {
    const auto it = m_items.find(id);
    if (it == m_items.end()) return std::nullopt;
    return it->second;
}

std::vector<DeviceId> Network::ids() const {
    std::vector<DeviceId> out;
    out.reserve(m_items.size());
    for (const auto& [id, v] : m_items) out.push_back(id);
    return out;
}

// ── Choices ────────────────────────────────────────────────────────────────

std::optional<Choice> Choices::get(const Id& id) const {
    const auto it = m_items.find(id);
    if (it == m_items.end()) return std::nullopt;
    return it->second;
}

void Choices::put(const Id& id, const std::optional<Choice>& v) {
    // The gate: a write with the door shut, outside a load, is a stray.
    if (m_loading == 0 && m_door && !m_door()) ++m_strays;
    if (v && !(*v == Choice{})) m_items[id] = *v;
    else                        m_items.erase(id);
    if (m_changed) m_changed();
}

// ── persistence ────────────────────────────────────────────────────────────

int load_choices(const std::string& file, Choices& into) {
    std::ifstream f(file);
    if (!f) return 0;   // first run
    int n = 0;
    try {
        nlohmann::json j;
        f >> j;
        if (!j.contains("devices") || !j["devices"].is_object()) return 0;
        Choices::Loading load(into);
        for (const auto& [id, e] : j["devices"].items()) {
            if (id.empty() || !e.is_object()) continue;
            Choice c;
            if (e.contains("nickname")  && e["nickname"].is_string())   c.nickname  = e["nickname"].get<std::string>();
            if (e.contains("favourite") && e["favourite"].is_boolean()) c.favourite = e["favourite"].get<bool>();
            if (e.contains("hidden")    && e["hidden"].is_boolean())    c.hidden    = e["hidden"].get<bool>();
            if (e.contains("pins") && e["pins"].is_array())
                for (const auto& p : e["pins"])
                    if (p.is_object() && p.contains("uri") && p["uri"].is_string() && !p["uri"].get<std::string>().empty())
                        c.pins.push_back({p.contains("name") && p["name"].is_string() ? p["name"].get<std::string>() : "",
                                          p["uri"].get<std::string>()});
            if (c == Choice{}) continue;
            into.put(id, c);
            ++n;
        }
    } catch (const std::exception&) {
        return n;   // unparseable: keep what was read, never throw across the seam
    }
    return n;
}

bool save_choices(const std::string& file, const Choices& from) {
    namespace fs = std::filesystem;
    nlohmann::json devs = nlohmann::json::object();
    for (const auto& [id, c] : from.all()) {
        nlohmann::json e = nlohmann::json::object();
        if (!c.nickname.empty()) e["nickname"]  = c.nickname;
        if (c.favourite)         e["favourite"] = true;
        if (c.hidden)            e["hidden"]    = true;
        if (!c.pins.empty()) {
            nlohmann::json ps = nlohmann::json::array();
            for (const auto& p : c.pins) ps.push_back({{"name", p.name}, {"uri", p.uri}});
            e["pins"] = ps;
        }
        devs[id] = e;
    }
    nlohmann::json j;
    j["version"] = 1;
    j["devices"] = devs;
    try {
        fs::create_directories(fs::path(file).parent_path());
        const std::string tmp = file + ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            if (!f) return false;
            f << j.dump(2) << "\n";
            if (!f) return false;
        }
        fs::rename(tmp, file);   // a crash mid-write leaves the old file whole
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

// ── the join ───────────────────────────────────────────────────────────────

std::string service_label(const std::string& kind) {
    if (kind == "device-info" || kind == "adisk") return {};
    if (kind == "afp")  return "AFP";
    if (kind == "upnp") return "UPnP";
    if (kind == "smb")  return "SMB";
    if (kind == "nfs")  return "NFS";
    if (kind == "ssh")  return "SSH";
    if (kind == "sftp") return "SFTP";
    if (kind == "http" || kind == "https") return "Web";
    if (kind == "ipp" || kind == "ipps" || kind == "lpd" || kind == "pdl") return "Printer";
    if (kind == "uscan" || kind == "uscans" || kind == "scanner") return "Scanner";
    return kind;
}

std::string service_detail(const std::string& kind) {
    if (kind == "ipp")     return "Printer (IPP)";
    if (kind == "ipps")    return "Printer (IPPS)";
    if (kind == "lpd")     return "Printer (LPD)";
    if (kind == "pdl")     return "Printer (raw)";
    if (kind == "uscan")   return "Scanner (eSCL)";
    if (kind == "uscans")  return "Scanner (eSCL, TLS)";
    if (kind == "scanner") return "Scanner";
    if (kind == "https")   return "Web (TLS)";
    return service_label(kind);
}

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}
}  // namespace

std::vector<Row> rows(const Network& net, const Choices& ch, bool show_hidden) {
    std::vector<Row> out;
    for (const auto& id : net.ids()) {
        const auto seen = net.get(id);
        const Choice c  = ch.of(id);
        if (c.hidden && !show_hidden) continue;
        const Sighting& s = seen->sighting;
        Row r;
        r.id         = id;
        r.advertised = s.name.empty() ? id : s.name;
        r.title      = c.nickname.empty() ? r.advertised : c.nickname;
        // "ZYXEL EMG6726-B10A · ZYXEL EMG6726-B10A" says it once.
        r.model      = lower(trim(s.model)) == lower(trim(r.advertised)) ? std::string{} : s.model;
        r.address    = s.addresses.empty() ? std::string{} : s.addresses.front();
        for (const auto& svc : s.services) {
            const auto l = service_label(svc.kind);
            if (l.empty() || r.services.find(l) != std::string::npos) continue;
            if (!r.services.empty()) r.services += ", ";
            r.services += l;
        }
        r.presence  = seen->presence;
        r.favourite = c.favourite;
        r.hidden    = c.hidden;
        out.push_back(std::move(r));
    }
    std::stable_sort(out.begin(), out.end(), [](const Row& a, const Row& b) {
        if (a.favourite != b.favourite) return a.favourite;
        if (a.presence != b.presence)   return a.presence == Presence::Online;
        const auto la = lower(a.title), lb = lower(b.title);
        if (la != lb) return la < lb;
        return a.id < b.id;
    });
    return out;
}

// ── the verbs ──────────────────────────────────────────────────────────────

namespace devices {

namespace {
// "Rename “tc01”" -- the step names the device as the list showed it.
std::string quoted(const Choices& c, const Network& n, const DeviceId& id) {
    const auto nick = c.of(id).nickname;
    if (!nick.empty()) return "“" + nick + "”";
    const auto s = n.get(id);
    return "“" + (s && !s->sighting.name.empty() ? s->sighting.name : id) + "”";
}

// One field of one device's choice, through the door. No change: no step.
template <class Edit>
bool change(DeviceJournal& j, Choices& c, const Network& n, const DeviceId& id,
            const std::string& label, Edit edit) {
    if (!n.get(id)) return false;   // choices are about devices the network knows
    Choice v = c.of(id);
    const Choice was = v;
    edit(v);
    if (v == was) return false;
    return j.run(label, {id}, [&] { c.put(id, v); });
}
}  // namespace

bool rename(DeviceJournal& j, Choices& c, const Network& n, const DeviceId& id, const std::string& nickname) {
    const std::string nick = trim(nickname);
    const std::string label = (nick.empty() ? "Clear nickname of " : "Rename ") + quoted(c, n, id);
    return change(j, c, n, id, label, [&](Choice& v) { v.nickname = nick; });
}

bool set_favourite(DeviceJournal& j, Choices& c, const Network& n, const DeviceId& id, bool on) {
    const std::string label = (on ? "Favourite " : "Unfavourite ") + quoted(c, n, id);
    return change(j, c, n, id, label, [&](Choice& v) { v.favourite = on; });
}

bool set_hidden(DeviceJournal& j, Choices& c, const Network& n, const DeviceId& id, bool on) {
    const std::string label = (on ? "Hide " : "Show ") + quoted(c, n, id);
    return change(j, c, n, id, label, [&](Choice& v) { v.hidden = on; });
}

bool is_pinned(const Choices& c, const DeviceId& id, const std::string& uri) {
    const auto pins = c.of(id).pins;
    return std::any_of(pins.begin(), pins.end(), [&](const Pin& p) { return p.uri == uri; });
}

bool set_pinned(DeviceJournal& j, Choices& c, const Network& n, const DeviceId& id, const Pin& share, bool on) {
    if (share.uri.empty()) return false;
    const std::string what = "“" + (share.name.empty() ? share.uri : share.name) + "”";
    const std::string label = (on ? "Pin " : "Unpin ") + what + " on " + quoted(c, n, id);
    return change(j, c, n, id, label, [&](Choice& v) {
        auto it = std::find_if(v.pins.begin(), v.pins.end(), [&](const Pin& p) { return p.uri == share.uri; });
        if (on && it == v.pins.end()) v.pins.push_back(share);
        if (!on && it != v.pins.end()) v.pins.erase(it);
    });
}

const std::vector<Verb>& verbs() {
    static const std::vector<Verb> table = {
        {"rename",      [](DeviceJournal& j, Choices& c, const Network& n, const DeviceId& f) { rename(j, c, n, f, "Basement"); }},
        {"favourite",   [](DeviceJournal& j, Choices& c, const Network& n, const DeviceId& f) { set_favourite(j, c, n, f, true); }},
        {"hide",        [](DeviceJournal& j, Choices& c, const Network& n, const DeviceId& f) { set_hidden(j, c, n, f, true); }},
        {"pin",         [](DeviceJournal& j, Choices& c, const Network& n, const DeviceId& f) {
            set_pinned(j, c, n, f, Pin{"pub", "smb://" + f + "/pub/"}, true); }},
    };
    return table;
}

}  // namespace devices
}  // namespace netscope::core
