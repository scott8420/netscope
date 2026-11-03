// netdiag_selftest -- the checks, headless. Routes reason over made-up Facts;
// ports are tried against a socket this test opens on 127.0.0.1.
#include "netdiag/Checks.hpp"

#include <arpa/inet.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nd = netscope::netdiag;
namespace core = netscope::core;

namespace {
int g_pass = 0, g_fail = 0;
void check(const std::string& name, bool ok, const std::string& detail = "") {
    (ok ? ++g_pass : ++g_fail);
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << name;
    if (!detail.empty()) std::cout << "  (" << detail << ")";
    std::cout << "\n";
}
core::Sighting dev(std::vector<std::string> addrs, std::vector<core::Service> svcs = {}) {
    return {"tc01.local", "tc01", "", std::move(addrs), std::move(svcs)};
}
}  // namespace

int main() {
    std::cout << "netdiag_selftest -- address, route, port, smb\n";
    using V = nd::Verdict;

    check("kind: 169.254 is link-local", nd::kind_of("169.254.192.51") == nd::Kind::LinkLocal);
    check("kind: 192.168 private", nd::kind_of("192.168.1.2") == nd::Kind::Private);
    check("kind: 172.20 private, 172.32 public", nd::kind_of("172.20.0.1") == nd::Kind::Private &&
          nd::kind_of("172.32.0.1") == nd::Kind::Public);
    check("kind: fe80:: is v6", nd::kind_of("fe80::1") == nd::Kind::V6);

    auto f = nd::check_address(dev({"169.254.192.51"}));
    check("address: self-assigned warns, plainly", f.verdict == V::Warn &&
          f.says.find("self-assigned") != std::string::npos, f.says);
    check("address: none fails", nd::check_address(dev({})).verdict == V::Fail);
    check("address: IPv4 preferred over v6", nd::check_address(dev({"fe80::1", "192.168.1.9"})).verdict == V::Ok);

    // Scott's Nov 3 laptop: wld0 on the home network, a default route, no 169.254.
    nd::Facts home{{{"wld0", "192.168.1.20", 24}}, true};
    f = nd::check_route(dev({"169.254.192.51"}), home);
    check("route: tc01's case -- no 169.254 here, Fail", f.verdict == V::Fail &&
          f.says.find("no 169.254 address") != std::string::npos, f.says);
    check("route: tc01's case offers the link-local fix", f.fix == "link-local");
    check("route: other failures offer no fix", nd::check_route(dev({"10.9.8.7"}), nd::Facts{home.ifaces, false}).fix.empty());
    nd::Facts fixed = home;
    fixed.ifaces.push_back({"wld0", "169.254.10.10", 16});
    f = nd::check_route(dev({"169.254.192.51"}), fixed);
    check("route: after the workaround, direct on wld0", f.verdict == V::Ok &&
          f.says.find("directly on wld0") != std::string::npos, f.says);
    check("route: same subnet direct", nd::check_route(dev({"192.168.1.30"}), home).says.find("directly") != std::string::npos);
    check("route: elsewhere via the gateway", nd::check_route(dev({"10.9.8.7"}), home).verdict == V::Ok);
    check("route: elsewhere and no gateway fails", nd::check_route(dev({"10.9.8.7"}), nd::Facts{home.ifaces, false}).verdict == V::Fail);
    check("route: no IPv4 skips", nd::check_route(dev({"fe80::1"}), home).verdict == V::Skip);

    // A real listening socket, and a port nothing listens on.
    const int ls = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(ls, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
    listen(ls, 1);
    socklen_t len = sizeof sa;
    getsockname(ls, reinterpret_cast<sockaddr*>(&sa), &len);
    const int open_port = ntohs(sa.sin_port);
    f = nd::check_port("127.0.0.1", open_port, "afp", std::chrono::milliseconds(1000));
    check("port: listening answers", f.verdict == V::Ok && f.says.find("AFP answers") != std::string::npos, f.says);
    close(ls);
    f = nd::check_port("127.0.0.1", open_port, "afp", std::chrono::milliseconds(1000));
    check("port: closed is refused", f.verdict == V::Fail && f.says.find("refused") != std::string::npos, f.says);
    // 192.0.2.0/24 is TEST-NET-1: never routed, so a connect hangs or is unreachable.
    f = nd::check_port("192.0.2.1", 548, "afp", std::chrono::milliseconds(300));
    check("port: no answer fails within the timeout", f.verdict == V::Fail, f.says);

    auto all = nd::diagnose(dev({"169.254.192.51"}, {{"afp", 548}}), home, std::chrono::milliseconds(300));
    check("diagnose: address, route, then one per port", all.size() == 3 && all[0].check == "address" &&
          all[1].check == "route" && all[2].check == "port 548");
    all = nd::diagnose(dev({}, {{"smb", 445}}), home);
    check("diagnose: no address -- ports skipped, not tried", all.size() == 3 && all[2].verdict == V::Skip);

    // ── SMB (milestone 6) ──────────────────────────────────────────────────
    auto req = nd::smb2_negotiate_request();
    check("smb: request is NetBIOS + 64-byte header + 36 + 4 dialects", req.size() == 4 + 64 + 36 + 8 &&
          req[0] == 0 && req[3] == 108 && req[4] == 0xFE && req[5] == 'S');
    auto reply = [](uint32_t status, int structure, int dialect) {
        std::vector<unsigned char> b{0, 0, 0, 0, 0xFE, 'S', 'M', 'B'};
        b.resize(4 + 64 + 8, 0);
        for (int i = 0; i < 4; ++i) b[4 + 8 + i] = (status >> (8 * i)) & 0xFF;
        b[68] = structure & 0xFF; b[69] = structure >> 8;
        b[72] = dialect & 0xFF;   b[73] = dialect >> 8;
        return b;
    };
    auto p = nd::parse_smb2_negotiate(reply(0, 65, 0x0302));
    check("smb: a 3.0.2 reply reads as 3.0.2", p.smb2 && p.dialect == 0x0302 && nd::dialect_name(p.dialect) == "SMB 3.0.2");
    p = nd::parse_smb2_negotiate(reply(0xC0000022, 65, 0));
    check("smb: a refusal says so, with its status", !p.smb2 && p.why.find("0xC0000022") != std::string::npos, p.why);
    p = nd::parse_smb2_negotiate({0, 0, 0, 0x20, 0xFF, 'S', 'M', 'B'});
    check("smb: an SMB1 reply is called old", p.smb1_only && !p.smb2);
    p = nd::parse_smb2_negotiate({'H', 'T', 'T', 'P', '/', '1', '.', '1'});
    check("smb: not SMB is not SMB", !p.smb2 && !p.smb1_only && p.why == "it isn't SMB");
    p = nd::parse_smb2_negotiate({0, 0});
    check("smb: too short says so", !p.smb2 && p.why.find("short") != std::string::npos);

    // A live server, when this machine runs one (the sandbox does; Scott's may not).
    f = nd::check_port("127.0.0.1", 445, "smb", std::chrono::milliseconds(500));
    if (f.verdict == V::Ok) {
        f = nd::check_smb("127.0.0.1", 445, std::chrono::milliseconds(2000));
        check("smb: the local server answers with a dialect", f.verdict == V::Ok &&
              f.says.find("SMB ") != std::string::npos, f.says);
    } else {
        std::cout << "  - smb: no server on 127.0.0.1:445, live check skipped\n";
    }

    std::cout << "-----------------------------------------------\n"
              << g_pass << " pass / " << g_fail << " fail\n";
    return g_fail == 0 ? 0 : 1;
}
