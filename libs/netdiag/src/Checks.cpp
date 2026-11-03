#include "netdiag/Checks.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace netscope::netdiag {

namespace {
bool parse4(const std::string& a, uint32_t& host_order) {
    in_addr in{};
    if (inet_pton(AF_INET, a.c_str(), &in) != 1) return false;
    host_order = ntohl(in.s_addr);
    return true;
}

std::string first_v4(const core::Sighting& s) {
    uint32_t x;
    for (const auto& a : s.addresses)
        if (parse4(a, x) && kind_of(a) != Kind::Loopback) return a;
    return {};
}

std::string who(const core::Sighting& s) { return s.name.empty() ? s.id : s.name; }

std::string label_of(const std::string& kind) {
    const auto l = core::service_label(kind);
    return l.empty() ? kind : l;
}
}  // namespace

Kind kind_of(const std::string& addr) {
    if (addr.empty()) return Kind::None;
    uint32_t a;
    if (!parse4(addr, a)) return addr.find(':') != std::string::npos ? Kind::V6 : Kind::None;
    if ((a >> 24) == 127)                 return Kind::Loopback;
    if ((a >> 16) == 0xA9FE)              return Kind::LinkLocal;   // 169.254/16
    if ((a >> 24) == 10)                  return Kind::Private;
    if ((a >> 20) == 0xAC1)               return Kind::Private;     // 172.16/12
    if ((a >> 16) == 0xC0A8)              return Kind::Private;     // 192.168/16
    return Kind::Public;
}

Facts local_facts() {
    Facts f;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
            char buf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, buf, sizeof buf);
            int prefix = 0;
            if (i->ifa_netmask) {
                uint32_t m = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr);
                while (m & 0x80000000u) { ++prefix; m <<= 1; }
            }
            f.ifaces.push_back({i->ifa_name, buf, prefix});
        }
        freeifaddrs(list);
    }
    // Destination 00000000 on an up route = a default route.
    std::ifstream rt("/proc/net/route");
    std::string line;
    std::getline(rt, line);   // header
    while (std::getline(rt, line)) {
        std::istringstream ss(line);
        std::string ifname, dest;
        if (ss >> ifname >> dest && dest == "00000000") { f.default_route = true; break; }
    }
    return f;
}

Finding check_address(const core::Sighting& s) {
    Finding r{"address", Verdict::Ok, {}};
    if (s.addresses.empty()) {
        r.verdict = Verdict::Fail;
        r.says = who(s) + " announced itself but reported no address.";
        return r;
    }
    const std::string a = first_v4(s).empty() ? s.addresses.front() : first_v4(s);
    switch (kind_of(a)) {
    case Kind::LinkLocal:
        r.verdict = Verdict::Warn;
        r.says = who(s) + " has a self-assigned address (" + a + "): it didn't get one from the router.";
        break;
    case Kind::Private:
        r.says = who(s) + " has a local address from the network (" + a + ").";
        break;
    case Kind::Public:
        r.says = who(s) + " has a public address (" + a + ").";
        break;
    case Kind::V6:
        r.verdict = Verdict::Warn;
        r.says = who(s) + " reported only an IPv6 address (" + a + "); the checks below need IPv4.";
        break;
    case Kind::Loopback:
        r.says = who(s) + " is this machine (" + a + ").";
        break;
    case Kind::None:
        r.verdict = Verdict::Fail;
        r.says = who(s) + " reported an address that can't be read (" + a + ").";
        break;
    }
    return r;
}

Finding check_route(const core::Sighting& s, const Facts& f) {
    Finding r{"route", Verdict::Ok, {}};
    const std::string a = first_v4(s);
    uint32_t target;
    if (a.empty() || !parse4(a, target)) {
        r.verdict = Verdict::Skip;
        r.says = "No IPv4 address to find a route to.";
        return r;
    }
    for (const auto& i : f.ifaces) {
        uint32_t mine;
        if (!parse4(i.addr, mine) || i.prefix <= 0) continue;
        const uint32_t mask = i.prefix >= 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> i.prefix);
        if ((mine & mask) == (target & mask)) {
            r.says = "Reachable directly on " + i.name + " (this machine is " + i.addr + "/" +
                     std::to_string(i.prefix) + ").";
            return r;
        }
    }
    if (kind_of(a) == Kind::LinkLocal) {
        r.verdict = Verdict::Fail;
        r.says = "No route: this machine has no 169.254 address, so it can't reach " + who(s) +
                 " even though it can see it. Adding one fixes that until the next reconnect.";
        return r;
    }
    if (f.default_route) {
        r.says = "Through the default gateway: " + who(s) + " is not on this machine's own subnet.";
        return r;
    }
    r.verdict = Verdict::Fail;
    r.says = "No route: " + a + " is not on any of this machine's networks and there is no default gateway.";
    return r;
}

Finding check_port(const std::string& addr, int port, const std::string& service,
                   std::chrono::milliseconds timeout) {
    const std::string name = label_of(service);
    Finding r{"port " + std::to_string(port), Verdict::Ok, {}};
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, addr.c_str(), &sa.sin_addr) != 1 || port <= 0) {
        r.verdict = Verdict::Skip;
        r.says = name + ": nothing to try.";
        return r;
    }
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        r.verdict = Verdict::Skip;
        r.says = std::string("Couldn't open a socket: ") + std::strerror(errno);
        return r;
    }
    int err = 0;
    if (connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        err = errno;
        if (err == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            const int n = poll(&p, 1, static_cast<int>(timeout.count()));
            if (n == 0) err = ETIMEDOUT;
            else if (n < 0) err = errno;
            else { socklen_t len = sizeof err; getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len); }
        }
    }
    close(fd);
    const std::string secs = std::to_string(timeout.count() / 1000.0).substr(0, 3);
    switch (err) {
    case 0:            r.says = name + " answers on port " + std::to_string(port) + "."; break;
    case ECONNREFUSED: r.verdict = Verdict::Fail;
                       r.says = "Port " + std::to_string(port) + " refused: " + name + " isn't listening."; break;
    case ETIMEDOUT:    r.verdict = Verdict::Fail;
                       r.says = "No answer on port " + std::to_string(port) + " within " + secs +
                                " s: asleep, filtered, or no route."; break;
    case ENETUNREACH:
    case EHOSTUNREACH: r.verdict = Verdict::Fail;
                       r.says = "Port " + std::to_string(port) + ": " + std::strerror(err) + "."; break;
    default:           r.verdict = Verdict::Fail;
                       r.says = "Port " + std::to_string(port) + ": " + std::strerror(err) + "."; break;
    }
    return r;
}

std::vector<Finding> diagnose(const core::Sighting& s, const Facts& f, std::chrono::milliseconds timeout) {
    std::vector<Finding> out{check_address(s), check_route(s, f)};
    const std::string a = first_v4(s);
    for (const auto& svc : s.services) {
        if (svc.port <= 0) continue;
        if (a.empty()) {
            out.push_back({"port " + std::to_string(svc.port), Verdict::Skip, label_of(svc.kind) + ": no IPv4 address to try."});
            continue;
        }
        out.push_back(check_port(a, svc.port, svc.kind, timeout));
    }
    return out;
}

}  // namespace netscope::netdiag
