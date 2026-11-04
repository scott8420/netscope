// netmount_selftest -- where shares live, headless. No bus, no gvfs.
#include "netmount/Nfs.hpp"
#include "netmount/Places.hpp"

#include <iostream>

namespace {
int g_pass = 0, g_fail = 0;
void check(const std::string& name, bool ok, const std::string& detail = "") {
    (ok ? g_pass : g_fail)++;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << name;
    if (!detail.empty()) std::cout << "  (" << detail << ")";
    std::cout << "\n";
}
}  // namespace

int main() {
    namespace nm = netscope::netmount;
    using netscope::core::Sighting;
    std::cout << "netmount_selftest -- shares as URIs\n";

    Sighting raspi{"raspi.local", "raspi", "MacSamba", {"192.168.1.165", "fe80::1"}, {{"smb", 445}, {"device-info", 0}}};
    Sighting tc01{"tc01.local", "tc01", "TimeCapsule8,119", {"169.254.192.51"}, {{"afp", 548}, {"smb", 445}}};
    Sighting nameless{"", "", "", {"fe80::2", "10.0.0.7"}, {{"sftp", 22}, {"nfs", 2049}}};

    check("scheme: smb, sftp, nfs through GIO", nm::scheme_for("smb") == "smb" && nm::scheme_for("sftp") == "sftp" &&
          nm::scheme_for("nfs") == "nfs");
    check("scheme: AFP is not GIO's (milestone 7)", nm::scheme_for("afp").empty());
    check("schemes: raspi has smb only", nm::schemes_of(raspi) == std::vector<std::string>{"smb"});
    check("schemes: tc01 -- smb, AFP left out", nm::schemes_of(tc01) == std::vector<std::string>{"smb"});
    check("schemes: fixed order smb, sftp, nfs", nm::schemes_of(nameless) == std::vector<std::string>{"sftp", "nfs"});

    check("host: the advertised name, not the address", nm::host_for(raspi) == "raspi.local");
    check("host: upper case folded", nm::host_for(Sighting{"RASPI.local", "", "", {}, {}}) == "raspi.local");
    check("host: no name -> first IPv4", nm::host_for(nameless) == "10.0.0.7");
    check("host: IPv6 only -> bracketed", nm::host_for(Sighting{"", "", "", {"fe80::2"}, {}}) == "[fe80::2]");

    check("root: smb://raspi.local/", nm::root_uri("smb", "raspi.local") == "smb://raspi.local/");
    check("share: plain name", nm::share_uri("smb", "raspi.local", "Public") == "smb://raspi.local/Public/");
    auto u = nm::share_uri("smb", "raspi.local", "Time Machine");
    check("share: a space is escaped", u == "smb://raspi.local/Time%20Machine/", u);

    check("admin: IPC$ and print$ are admin", nm::is_admin_share("IPC$") && nm::is_admin_share("print$"));
    check("admin: Public is not", !nm::is_admin_share("Public"));

    check("normal: case-blind host, one slash", nm::normal("SMB://RASPI.local/Public") == "smb://raspi.local/Public/");
    check("normal: the share's case is kept", nm::normal("smb://raspi.local/Public//") != nm::normal("smb://raspi.local/public/"));
    std::vector<std::string> roots{"file:///", "smb://raspi.local/Public/", "sftp://nas.local/"};
    check("mounted: found without the trailing slash", nm::is_mounted("smb://raspi.local/Public", roots));
    check("mounted: another share isn't", !nm::is_mounted("smb://raspi.local/Music/", roots));

    // ── Milestone 14: SFTP and NFS ──────────────────────────────────────────
    check("schemes: plain ssh offers sftp", nm::schemes_of(Sighting{"pi.local", "", "", {}, {{"smb", 445}, {"ssh", 22}}}) ==
          std::vector<std::string>{"smb", "sftp"});
    check("sftp: one share, the server", nm::sftp_share("raspi.local") == nm::Share{"Files (SFTP)", "sftp://raspi.local/"});
    {
        namespace nfs = nm::nfs;
        auto u = nfs::export_uri("raspi.local", "/srv/nfs/photos");
        check("nfs: export URI keeps slashes", u == "nfs://raspi.local/srv/nfs/photos/", u);
        u = nfs::export_uri("raspi.local", "/srv/My Music/");
        check("nfs: parts escaped", u == "nfs://raspi.local/srv/My%20Music/", u);
        check("nfs: root export", nfs::export_uri("nas.local", "/") == "nfs://nas.local/");
        check("nfs: names", nfs::export_name("/srv/nfs/photos") == "photos" && nfs::export_name("/") == "/" &&
              nfs::export_name("/data/") == "data");
        check("nfs: mounted root (gio has no trailing slash) matches",
              nm::is_mounted(nfs::export_uri("127.0.0.1", "/srv/nfs/photos"), {"nfs://127.0.0.1/srv/nfs/photos"}));

        // The wire: a GETPORT call is 56 bytes; replies built by hand.
        const auto c = nfs::call_getport(7, nfs::kMountd, 3, nfs::kTcp);
        check("rpc: GETPORT body is 56 bytes", c.size() == 56, std::to_string(c.size()));
        const auto rec = nfs::record(c);
        check("rpc: record mark = last fragment | 56", rec.size() == 60 && (unsigned char)rec[0] == 0x80 && rec[3] == 56);
        check("rpc: defragment round trip", nfs::defragment(rec) == c);
        check("rpc: defragment waits for the rest", !nfs::defragment(rec.substr(0, 30)).has_value());
        auto w = [](std::string& o, std::uint32_t v) {
            for (int i = 3; i >= 0; --i) o += static_cast<char>((v >> (8 * i)) & 0xff);
        };
        auto ws = [&](std::string& o, const std::string& t) {
            w(o, static_cast<std::uint32_t>(t.size()));
            o += t;
            while (o.size() % 4) o += '\0';
        };
        auto head = [&](std::uint32_t xid) { std::string o; w(o, xid); w(o, 1); w(o, 0); w(o, 0); w(o, 0); w(o, 0); return o; };
        std::string gp = head(7); w(gp, 20048);
        check("rpc: GETPORT reply -> 20048", nfs::parse_getport(gp, 7) == 20048u);
        check("rpc: another xid is not ours", !nfs::parse_getport(gp, 8).has_value());
        std::string ex = head(9);
        w(ex, 1); ws(ex, "/srv/nfs/photos"); w(ex, 1); ws(ex, "*"); w(ex, 0);
        w(ex, 1); ws(ex, "/srv/nfs/music"); w(ex, 0);
        w(ex, 0);
        const auto got = nfs::parse_exports(ex, 9);
        check("rpc: EXPORT reply -> two exports, groups kept", got && got->size() == 2 &&
              (*got)[0] == nfs::Export{"/srv/nfs/photos", {"*"}} && (*got)[1] == nfs::Export{"/srv/nfs/music", {}});
        std::string empty = head(10); w(empty, 0);
        check("rpc: no exports -> empty list, not a failure", nfs::parse_exports(empty, 10) == std::vector<nfs::Export>{});
        check("rpc: cut short -> nothing", !nfs::parse_exports(ex.substr(0, ex.size() - 8), 9).has_value());
        std::string denied = head(11); denied.resize(20); w(denied, 1);   // accept_stat PROG_UNAVAIL
        check("rpc: PROG_UNAVAIL -> nothing", !nfs::parse_getport(denied, 11).has_value());
    }

    std::cout << "-----------------------------------------------\n"
              << g_pass << " pass / " << g_fail << " fail\n";
    return g_fail == 0 ? 0 : 1;
}
