// netafp_selftest -- AFP volumes as URIs and mount points, headless.
#include "netafp/Afp.hpp"

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
    namespace na = netscope::netafp;
    std::cout << "netafp_selftest -- AFP volumes, mount points, mountinfo\n";

    check("uri: afp://tc01.local/Data/", na::volume_uri("tc01.local", "Data") == "afp://tc01.local/Data/");
    const auto tm = na::volume_uri("tc01.local", "Time Machine");
    check("uri: a space is escaped", tm == "afp://tc01.local/Time%20Machine/", tm);
    check("is_afp: afp yes, smb no", na::is_afp_uri(tm) && !na::is_afp_uri("smb://raspi.local/pub/"));
    std::string h, v;
    check("split: host and volume back", na::split_uri(tm, h, v) && h == "tc01.local" && v == "Time Machine", h + " / " + v);
    check("split: no trailing slash is fine", na::split_uri("afp://tc01.local/Data", h, v) && v == "Data");
    check("split: server root is not a volume", !na::split_uri("afp://tc01.local/", h, v));
    check("split: a path below a volume is not a volume", !na::split_uri("afp://tc01.local/Data/Backup/", h, v));
    check("split: smb refused", !na::split_uri("smb://raspi.local/pub/", h, v));

    check("short host: .local dropped", na::short_host("tc01.local") == "tc01");
    check("short host: address kept", na::short_host("169.254.192.51") == "169.254.192.51");
    check("mount point: ~/tc01/Data", na::mountpoint_for("/home/scott", "tc01.local", "Data") == "/home/scott/tc01/Data");
    check("mount point: a slash in a volume name can't escape",
          na::mountpoint_for("/home/scott", "tc01.local", "a/../b") == "/home/scott/tc01/a-..-b");

    const std::string info =
        "22 1 0:21 / /proc rw,nosuid shared:12 - proc proc rw\n"
        "88 30 0:45 / /home/scott/tc01/Data rw,nosuid,nodev,relatime shared:50 - fuse.afpfs scott@tc01:Data rw,user_id=1000\n"
        "90 30 0:47 / /home/scott/tc01/Time\\040Machine rw,nosuid - fuse.afpfs scott@tc01:Time\\040Machine rw\n"
        "91 30 0:48 / /run/user/1000/gvfs rw,nosuid shared:51 - fuse.gvfsd-fuse gvfsd-fuse rw\n";
    const auto ms = na::parse_mountinfo(info);
    check("mountinfo: two afpfs mounts, not proc or gvfs", ms.size() == 2, std::to_string(ms.size()));
    check("mountinfo: point and source", ms.size() > 0 && ms[0].point == "/home/scott/tc01/Data" && ms[0].source == "scott@tc01:Data");
    check("mountinfo: \\040 decoded", ms.size() > 1 && ms[1].point == "/home/scott/tc01/Time Machine", ms.size() > 1 ? ms[1].point : "");
    check("mountinfo: empty text", na::parse_mountinfo("").empty());

    check("available() and missing() agree", na::available() == na::missing().empty(), na::missing());

    std::cout << "-----------------------------------------------\n"
              << g_pass << " pass / " << g_fail << " fail\n";
    return g_fail == 0 ? 0 : 1;
}
