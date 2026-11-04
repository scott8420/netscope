#pragma once
// netdiscover SSDP -- UPnP devices (milestone 13). Pure parts: no sockets, no
// threads, so the parsing and grouping rules are tested headless.
//
// SSDP (UPnP's discovery): a client multicasts an M-SEARCH to
// 239.255.255.250:1900; devices answer by unicast ("HTTP/1.1 200 OK" with
// LOCATION, USN, ST, SERVER, CACHE-CONTROL: max-age) and also announce
// themselves with NOTIFY ssdp:alive / ssdp:byebye. LOCATION points at an XML
// description: friendlyName, manufacturer, modelName, deviceType,
// presentationURL (a web admin page, often).
//
// One device answers several times (root device, embedded devices, each
// service type) -- all with the same uuid in USN. NetScope lists one device
// per uuid. Its id is "upnp-<uuid>" unless the Merger folds it into the Avahi
// device at the same address (Merge.hpp).
#include "core/Feed.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace netscope::netdiscover::ssdp {

inline constexpr const char* kGroup = "239.255.255.250";
inline constexpr int         kPort  = 1900;

// The M-SEARCH we send: every device and service ("ssdp:all"), answer within mx s.
std::string search_request(int mx = 2);

struct Reply {
    enum class Kind { Answer, Alive, Byebye };
    Kind        kind = Kind::Answer;
    std::string location;   // "http://192.168.1.20:49152/description.xml"
    std::string usn;        // "uuid:2fac1234-...::urn:schemas-upnp-org:device:MediaServer:1"
    std::string st;         // ST (answers) or NT (notifies)
    std::string server;     // "Linux/5.10 UPnP/1.0 MiniDLNA/1.3.0"
    int         max_age = 1800;
};
// An answer to our M-SEARCH, or a NOTIFY. nullopt for anything else
// (another client's M-SEARCH, garbage). Header names are case-insensitive.
std::optional<Reply> parse(const std::string& datagram);

// "uuid:2fac...::urn:..." -> "2fac..." (lower-case); "" if there is none.
std::string uuid_of(const std::string& usn);

struct Url { std::string host; int port = 80; std::string path = "/"; };
std::optional<Url> parse_url(const std::string& url);   // http:// only

struct Description {
    std::string friendly_name, manufacturer, model_name, model_number, device_type, presentation_url;
};
// The root <device> of a UPnP description (first occurrence of each tag; XML
// entities &amp; &lt; &gt; &quot; &apos; decoded). Empty fields when missing.
Description parse_description(const std::string& xml);

// "urn:schemas-upnp-org:device:MediaServer:1" -> "Media server"; "" if unknown.
std::string device_label(const std::string& device_type);

// ── Grouping: replies (+ descriptions) in, sightings out ────────────────────
class Aggregator {
public:
    using Clock = std::chrono::steady_clock;
    // An answer or ssdp:alive. Events to push, possibly none. `fetch` says
    // whether the caller should fetch this device's description now: the
    // first time a LOCATION is seen, and again (30 s apart) while it has none.
    std::vector<core::FeedEvent> reply(const Reply& r, Clock::time_point now, bool* fetch = nullptr);
    // A description arrived for uuid.
    std::vector<core::FeedEvent> described(const std::string& uuid, const Description& d);
    // Devices whose max-age ran out (no re-announce, no answer to a re-search).
    std::vector<core::FeedEvent> expire(Clock::time_point now);

    std::size_t devices() const { return m_devs.size(); }

private:
    struct Dev {
        std::string                location, server;
        std::optional<Description> desc;
        Clock::time_point          until;
        Clock::time_point          tried{};   // last description fetch asked for
    };
    core::Sighting build(const std::string& uuid, const Dev& d) const;
    std::vector<core::FeedEvent> report(const std::string& uuid);

    std::map<std::string, Dev>             m_devs;   // by uuid
    std::map<std::string, core::Sighting>  m_said;   // last Seen per uuid
};

core::DeviceId device_id(const std::string& uuid);   // "upnp-<uuid>"

}  // namespace netscope::netdiscover::ssdp
