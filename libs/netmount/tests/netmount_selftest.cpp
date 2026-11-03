// netmount_selftest -- where shares live, headless. No bus, no gvfs.
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

    std::cout << "-----------------------------------------------\n"
              << g_pass << " pass / " << g_fail << " fail\n";
    return g_fail == 0 ? 0 : 1;
}
