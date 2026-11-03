// netdiscover_selftest -- the Aggregator's grouping rules, headless.
#include "netdiscover/Aggregator.hpp"

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
    check("types: unknown maps to nothing", nd::kind_for_type("_printer._tcp").empty());
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

    std::cout << "-----------------------------------------------\n"
              << g_pass << " pass / " << g_fail << " fail\n";
    return g_fail == 0 ? 0 : 1;
}
