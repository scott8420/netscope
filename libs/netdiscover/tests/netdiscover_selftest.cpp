// netdiscover_selftest -- the Aggregator's grouping rules, headless.
#include "netdiscover/Aggregator.hpp"
#include "netdiscover/Merge.hpp"
#include "netdiscover/Ssdp.hpp"

#include <chrono>
#include <iostream>
#include <string>

namespace nd = netscope::netdiscover;
namespace core = netscope::core;

namespace {
int g_pass = 0, g_fail = 0;
void check(const std::string& name, bool ok, const std::string& detail = "") {
    (ok ? ++g_pass : ++g_fail);
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << name;
    if (!detail.empty()) std::cout << "  (" << detail << ")";
    std::cout << "\n";
}
nd::ServiceKey key(const char* name, const char* type, int proto = 0, int iface = 2) {
    return {iface, proto, name, type, "local"};
}
}  // namespace

int main() {
    std::cout << "netdiscover_selftest -- services in, devices out\n";
    using K = core::FeedEvent::Kind;

    check("types: AFP maps to afp", nd::kind_for_type("_afpovertcp._tcp") == "afp");
    check("types: unknown maps to nothing", nd::kind_for_type("_apple-mobdev2._tcp").empty());
    check("id: lower-case, no trailing dot", nd::device_id("TC01.local.") == "tc01.local");

    {
        nd::Aggregator a;
        auto e = a.resolved(key("tc01", "_afpovertcp._tcp"), {"tc01.local", "169.254.192.51", 548, {}});
        check("first service: one Seen", e.size() == 1 && e[0].kind == K::Seen);
        check("first service: the device", e[0].sighting.id == "tc01.local" && e[0].sighting.name == "tc01" &&
              e[0].sighting.addresses == std::vector<std::string>{"169.254.192.51"} &&
              e[0].sighting.services == std::vector<core::Service>{{"afp", 548}});

        e = a.resolved(key("tc01", "_afpovertcp._tcp"), {"tc01.local", "169.254.192.51", 548, {}});
        check("same again: silent", e.empty());

        e = a.resolved(key("tc01", "_afpovertcp._tcp", 1), {"tc01.local", "fe80::1", 548, {}});
        check("over IPv6 too: one device, v4 first",
              e.size() == 1 && e[0].sighting.addresses == std::vector<std::string>{"169.254.192.51", "fe80::1"} &&
              e[0].sighting.services.size() == 1);

        e = a.resolved(key("tc01", "_device-info._tcp"), {"tc01.local", "169.254.192.51", 0, {"model=TimeCapsule8,119"}});
        check("device-info: model, not a service", e.size() == 1 && e[0].sighting.model == "TimeCapsule8,119" &&
              e[0].sighting.services.size() == 1);

        e = a.resolved(key("tc01", "_adisk._tcp"), {"tc01.local", "169.254.192.51", 9, {}});
        check("adisk: an announcement, not a service", e.empty());

        e = a.resolved(key("RASPI", "_smb._tcp"), {"raspi.local", "192.168.1.30", 445, {}});
        check("another host: its own device", e.size() == 1 && e[0].sighting.id == "raspi.local");

        e = a.removed(key("tc01", "_afpovertcp._tcp", 1));
        check("one copy gone: still seen, v6 gone", e.size() == 1 && e[0].kind == K::Seen &&
              e[0].sighting.addresses == std::vector<std::string>{"169.254.192.51"});
        a.removed(key("tc01", "_afpovertcp._tcp"));
        a.removed(key("tc01", "_device-info._tcp"));
        check("an announcement alone keeps the device", a.services() == 2);
        e = a.removed(key("tc01", "_adisk._tcp"));
        check("last service gone: Lost", e.size() == 1 && e[0].kind == K::Lost && e[0].sighting.id == "tc01.local");
        check("unknown removal: silent", a.removed(key("nope", "_ssh._tcp")).empty());
        check("raspi untouched", a.services() == 1);

        e = a.resolved(key("RASPI", "_smb._tcp"), {"pi2.local", "192.168.1.31", 445, {}});
        check("a service changes host: old Lost, new Seen", e.size() == 2 &&
              e[0].kind == K::Lost && e[0].sighting.id == "raspi.local" &&
              e[1].kind == K::Seen && e[1].sighting.id == "pi2.local");
        a.resolved(key("me", "_ssh._tcp"), {"vm.local", "127.0.0.1", 22, {}});
        e = a.resolved(key("me", "_ssh._tcp", 0, 3), {"vm.local", "192.0.2.2", 22, {}});
        check("loopback goes last", e.size() == 1 &&
              e[0].sighting.addresses == std::vector<std::string>{"192.0.2.2", "127.0.0.1"});
        check("no host: ignored", a.resolved(key("x", "_ssh._tcp"), {"", "", 22, {}}).empty());
    }


    // ── Milestone 13: SSDP ────────────────────────────────────────────────
    {
        namespace sp = nd::ssdp;
        const std::string answer =
            "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nEXT:\r\n"
            "LOCATION: http://192.0.2.20:8200/rootDesc.xml\r\nSERVER: Linux/6.1 UPnP/1.0 MiniDLNA/1.3.3\r\n"
            "ST: urn:schemas-upnp-org:device:MediaServer:1\r\n"
            "USN: uuid:4D696E69-444C-164E-9D41-B827EB0A1B2C::urn:schemas-upnp-org:device:MediaServer:1\r\n\r\n";
        const auto r = sp::parse(answer);
        check("ssdp: an answer parses", r && r->kind == sp::Reply::Kind::Answer);
        check("ssdp: location, server, max-age", r && r->location == "http://192.0.2.20:8200/rootDesc.xml" &&
              r->server.find("MiniDLNA") != std::string::npos && r->max_age == 120);
        check("ssdp: uuid from USN, lower-case", sp::uuid_of(r ? r->usn : "") == "4d696e69-444c-164e-9d41-b827eb0a1b2c");
        check("ssdp: lower-case header names too", sp::parse("HTTP/1.1 200 OK\r\nlocation: http://h/\r\nusn: uuid:abc\r\n\r\n").has_value());
        const auto bye = sp::parse("NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nNT: upnp:rootdevice\r\n"
                                   "NTS: ssdp:byebye\r\nUSN: uuid:abc::upnp:rootdevice\r\n\r\n");
        check("ssdp: byebye needs no location", bye && bye->kind == sp::Reply::Kind::Byebye);
        check("ssdp: another client's M-SEARCH is not a reply", !sp::parse(sp::search_request()));
        check("ssdp: no uuid -> ignored", !sp::parse("HTTP/1.1 200 OK\r\nLOCATION: http://h/\r\nUSN: x\r\n\r\n"));
        const auto u = sp::parse_url("http://192.0.2.20:8200/rootDesc.xml");
        check("url: host, port, path", u && u->host == "192.0.2.20" && u->port == 8200 && u->path == "/rootDesc.xml");
        check("url: default port 80", sp::parse_url("http://router.lan")->port == 80);
        const auto d = sp::parse_description(
            "<?xml version=\"1.0\"?><root><specVersion><major>1</major></specVersion><device>"
            "<deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>"
            "<friendlyName>RASPI: minidlna</friendlyName><manufacturer>Justin Maggard</manufacturer>"
            "<modelName>Windows Media Connect compatible (MiniDLNA)</modelName>"
            "<presentationURL>http://192.0.2.20:8200/</presentationURL>"
            "<deviceList><device><friendlyName>inner</friendlyName></device></deviceList></device></root>");
        check("description: root friendlyName, not the embedded one", d.friendly_name == "RASPI: minidlna", d.friendly_name);
        check("description: device type -> label", sp::device_label(d.device_type) == "Media server");
        check("description: &amp; decoded", sp::parse_description("<device><friendlyName>A &amp; B</friendlyName></device>").friendly_name == "A & B");

        sp::Aggregator ag;
        const auto t0 = std::chrono::steady_clock::now();
        bool fetch = false;
        auto e = ag.reply(*r, t0, &fetch);
        check("group: first answer -> Seen upnp-<uuid>, fetch the description",
              e.size() == 1 && e[0].sighting.id == "upnp-4d696e69-444c-164e-9d41-b827eb0a1b2c" && fetch);
        check("group: address and UPnP port from LOCATION", e.size() == 1 &&
              e[0].sighting.addresses == std::vector<std::string>{"192.0.2.20"} &&
              e[0].sighting.services == std::vector<core::Service>{{"upnp", 8200}});
        check("group: no description yet -> named by its address", e.size() == 1 && e[0].sighting.name == "192.0.2.20",
              e.empty() ? "" : e[0].sighting.name);
        auto r2 = *r;
        r2.usn = "uuid:4D696E69-444C-164E-9D41-B827EB0A1B2C::upnp:rootdevice";
        check("group: same uuid again -> nothing new, no refetch yet", ag.reply(r2, t0, &fetch).empty() && !fetch);
        ag.reply(r2, t0 + std::chrono::seconds(31), &fetch);
        check("group: still no description 30 s on -> fetch again", fetch);
        {   // Scott's router: one uuid from two LOCATIONs -- no flip-flop once described.
            sp::Aggregator z;
            sp::Reply a = *r; a.location = "http://192.168.1.1:1991/a.xml";
            sp::Reply b = *r; b.location = "http://192.168.1.1:1992/b.xml";
            z.reply(a, t0, nullptr);
            sp::Description wfa; wfa.friendly_name = "WFADevice"; wfa.manufacturer = "ZYXEL"; wfa.model_name = "EMG6726-B10A";
            wfa.device_type = "urn:schemas-wifialliance-org:device:WFADevice:1";
            auto ev = z.described(sp::uuid_of(a.usn), wfa);
            check("router: WFADevice named by its model", ev.size() == 1 && ev[0].sighting.name == "ZYXEL EMG6726-B10A",
                  ev.empty() ? "" : ev[0].sighting.name);
            bool f2 = true;
            check("router: second LOCATION once described -> nothing, no refetch", z.reply(b, t0, &f2).empty() && !f2);
        }
        e = ag.described("4d696e69-444c-164e-9d41-b827eb0a1b2c", d);
        check("group: description -> name, model, Web from presentationURL", e.size() == 1 &&
              e[0].sighting.name == "RASPI: minidlna" &&
              e[0].sighting.model == "Justin Maggard Windows Media Connect compatible (MiniDLNA)" &&
              e[0].sighting.services.size() == 2 && e[0].sighting.services[1] == core::Service{"http", 8200});
        check("group: alive before max-age -> no expiry", ag.expire(t0 + std::chrono::seconds(150)).empty());
        e = ag.expire(t0 + std::chrono::seconds(152));   // the reply at +31 s carried it to +151
        check("group: max-age lapsed -> Lost", e.size() == 1 && e[0].kind == core::FeedEvent::Kind::Lost);
        ag.reply(*r, t0, nullptr);
        auto b = *bye; b.usn = r->usn;
        e = ag.reply(b, t0, nullptr);
        check("group: byebye -> Lost", e.size() == 1 && e[0].kind == core::FeedEvent::Kind::Lost && ag.devices() == 0);
    }
    // ── Milestone 13: merging Avahi and SSDP ───────────────────────────────
    {
        using K = core::FeedEvent::Kind;
        nd::Merger m;
        const core::Sighting raspi{"raspi.local", "RASPI", "", {"192.0.2.20"}, {{"smb", 445}, {"ssh", 22}}};
        const core::Sighting dlna{"upnp-abc", "RASPI: minidlna", "MiniDLNA", {"192.0.2.20"}, {{"http", 8200}, {"upnp", 8200}}};
        const core::Sighting router{"upnp-def", "Router", "Router", {"192.0.2.1"}, {{"upnp", 5000}}};
        auto e = m.take(nd::Merger::From::Ssdp, {K::Seen, dlna, {}});
        check("merge: UPnP alone -> listed under its own id", e.size() == 1 && e[0].sighting.id == "upnp-abc");
        e = m.take(nd::Merger::From::Avahi, {K::Seen, raspi, {}});
        bool lost = false, seen = false;
        for (const auto& x : e) {
            lost = lost || (x.kind == K::Merged && x.sighting.id == "upnp-abc");
            if (x.kind == K::Seen && x.sighting.id == "raspi.local") {
                seen = x.sighting.services.size() == 4 && x.sighting.model == "MiniDLNA" && x.sighting.name == "RASPI";
            }
        }
        check("merge: same address on Avahi -> folded in (Merged upnp id, Seen raspi with 4 services)", lost && seen && e.size() == 2);
        e = m.take(nd::Merger::From::Ssdp, {K::Seen, router, {}});
        check("merge: another address stays its own device", e.size() == 1 && e[0].sighting.id == "upnp-def");
        e = m.take(nd::Merger::From::Ssdp, {K::Lost, core::Sighting{"upnp-abc", {}, {}, {}, {}}, {}});
        check("merge: UPnP gone -> raspi back to its own services", e.size() == 1 && e[0].kind == K::Seen &&
              e[0].sighting == raspi);
        const core::Sighting r2{"upnp-zzz", "Router", "Router", {"192.0.2.1"}, {{"upnp", 5001}}};
        e = m.take(nd::Merger::From::Ssdp, {K::Seen, r2, {}});
        bool merged2 = false, one = false;
        for (const auto& x : e) {
            merged2 = merged2 || (x.kind == K::Merged && x.sighting.id == "upnp-zzz");
            one = one || (x.kind == K::Seen && x.sighting.id == "upnp-def" && x.sighting.services.size() == 2);
        }
        check("merge: two UPnP uuids at one address -> one row (lowest id)", (merged2 || e.size() == 1) && one);
        {
            // s006: Scott's 19:56 router -- the lowest uuid not yet described is
            // named by its address; it keeps the described sibling's name.
            nd::Merger m2;
            const core::Sighting named{"upnp-bbb", "ZYXEL EMG6726-B10A", "ZYXEL EMG6726-B10A", {"192.168.1.1"}, {{"upnp", 1991}}};
            const core::Sighting bare{"upnp-aaa", "192.168.1.1", "", {"192.168.1.1"}, {{"upnp", 1992}}};
            m2.take(nd::Merger::From::Ssdp, {K::Seen, named, {}});
            auto e2 = m2.take(nd::Merger::From::Ssdp, {K::Seen, bare, {}});
            bool kept = false;
            for (const auto& x : e2)
                if (x.kind == K::Seen && x.sighting.id == "upnp-aaa") kept = x.sighting.name == "ZYXEL EMG6726-B10A";
            check("merge: an undescribed uuid keeps its sibling's name, not the address", kept);
        }
        e = m.take(nd::Merger::From::Avahi, {K::Note, {}, "avahi says hi"});
        check("merge: notes pass through", e.size() == 1 && e[0].note == "avahi says hi");
    }

    // ── s006: printers and scanners (Scott's avahi-browse, Nov 3) ───────────
    {
        check("types: IPP / LPD / raw / eSCL browsed",
              nd::kind_for_type("_ipp._tcp") == "ipp" && nd::kind_for_type("_ipps._tcp") == "ipps" &&
              nd::kind_for_type("_printer._tcp") == "lpd" && nd::kind_for_type("_pdl-datastream._tcp") == "pdl" &&
              nd::kind_for_type("_uscan._tcp") == "uscan" && nd::kind_for_type("_uscans._tcp") == "uscans" &&
              nd::kind_for_type("_scanner._tcp") == "scanner");
        nd::Aggregator a;
        const char* bro = "BRW90489A9696AE.local";
        auto e = a.resolved(key("Brother HL-L2360D series", "_http._tcp"), {bro, "192.168.1.175", 80, {}});
        check("printer: web page alone -> named by host", e.size() == 1 && e[0].sighting.name == "BRW90489A9696AE");
        e = a.resolved(key("Brother HL-L2360D series", "_ipp._tcp"), {bro, "192.168.1.175", 631,
                       {"rp=ipp/print", "ty=Brother HL-L2360D series", "usb_MFG=Brother", "usb_MDL=HL-L2360D series",
                        "adminurl=http://BRW90489A9696AE.local./net/net/airprint.html"}});
        check("printer: IPP -> named by its instance, model from ty=", e.size() == 1 &&
              e[0].sighting.id == "brw90489a9696ae.local" && e[0].sighting.name == "Brother HL-L2360D series" &&
              e[0].sighting.model == "Brother HL-L2360D series",
              e.empty() ? "" : e[0].sighting.name + " / " + e[0].sighting.model);
        a.resolved(key("Brother HL-L2360D series", "_printer._tcp"), {bro, "192.168.1.175", 515, {"ty=Brother HL-L2360D series"}});
        e = a.resolved(key("Brother HL-L2360D series", "_pdl-datastream._tcp"), {bro, "192.168.1.175", 9100, {}});
        check("printer: IPP + LPD + raw + Web are four services", e.size() == 1 && e[0].sighting.services.size() == 4);

        const char* ep = "EPSON5D07A1.local";
        a.resolved(key("EPSON ET-8550 Series", "_uscan._tcp"), {ep, "192.168.1.221", 443, {"ty=EPSON ET-8550 Series"}});
        e = a.resolved(key("EPSON ET-8550 Series", "_ipps._tcp"), {ep, "192.168.1.221", 631, {"TLS=1.2", "rp=ipp/print", "ty=EPSON ET-8550 Series"}});
        check("printer + scanner: one device, named by its instance", e.size() == 1 && e[0].sighting.name == "EPSON ET-8550 Series");
        e = a.resolved(key("EPSON5D07A1", "_smb._tcp"), {ep, "192.168.1.221", 445, {}});
        check("printer: SMB doesn't take the name back", e.size() == 1 && e[0].sighting.name == "EPSON ET-8550 Series" &&
              e[0].sighting.services.size() == 3);
        e = a.resolved(key("EPSON5D07A1", "_device-info._tcp"), {ep, "192.168.1.221", 0, {"model=ET-8550"}});
        check("printer: device-info's model wins over ty=", e.size() == 1 && e[0].sighting.model == "ET-8550");
        e = a.resolved(key("iPhone", "_apple-mobdev2._tcp"), {"iphone.local", "192.168.1.50", 32498, {}});
        check("unknown type: the device has no services", e.size() == 1 && e[0].sighting.services.empty());
    }

    std::cout << "-----------------------------------------------\n"
              << g_pass << " pass / " << g_fail << " fail\n";
    return g_fail == 0 ? 0 : 1;
}
