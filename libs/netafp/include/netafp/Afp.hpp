#pragma once
// netafp -- AFP through Netatalk Client (milestone 10). No GTK.
//
// GVfs's AFP login fails against the Time Capsule (kFPAuthContinue); Netatalk
// Client 0.9.5 logs in (AFP_FINDINGS). So:
//   * volumes are listed through its library, libafpsl (talks to its afpsld
//     daemon) -- the password stays in this process;
//   * a volume is mounted by its `afp_client mount`, which starts afpfsd and
//     makes a FUSE mount (fuse.afpfs). The password reaches it through a
//     private pipe: with stdin not a terminal, afp_client runs `ssh-askpass`
//     from PATH, and ours (written to $XDG_RUNTIME_DIR/netscope, mode 700)
//     reads the pipe. Never on a command line, never in the environment.
//   * the mount point is ~/<host>/<volume> ("~/tc01/Data"): under home, so
//     Nautilus shows it in the sidebar with an eject button.
//
// Netatalk Client is optional at build time: without libafpsl, available()
// says so and nothing else is attempted.
//
// Pure parts (URIs, mount points, /proc mountinfo) are tested headless; the
// rest blocks: callers run it off the main thread.
#include <string>
#include <vector>

namespace netscope::netafp {

// ── Pure ────────────────────────────────────────────────────────────────────
// afp://tc01.local/Data/  <->  host "tc01.local", volume "Data"
std::string volume_uri(const std::string& host, const std::string& volume);
bool        is_afp_uri(const std::string& uri);
bool        split_uri(const std::string& uri, std::string& host, std::string& volume);   // %XX decoded
// "tc01.local" -> "tc01"; addresses kept as they are.
std::string short_host(const std::string& host);
// ~/tc01/Data (home passed in for tests). Volume slashes become "-".
std::string mountpoint_for(const std::string& home, const std::string& host, const std::string& volume);

struct Mount {
    std::string point;    // "/home/scott/tc01/Data" (\040 etc. decoded)
    std::string source;   // "scott@tc01:Data"
};
// The fuse.afpfs lines of a /proc/self/mountinfo text.
std::vector<Mount> parse_mountinfo(const std::string& text);

// ── System ──────────────────────────────────────────────────────────────────
bool        available();     // built with libafpsl AND afp_client found
std::string missing();       // why not, one sentence ("" when available)
std::vector<Mount> mounts(); // reads /proc/self/mountinfo
bool        is_mounted(const std::string& point);

struct Login {
    std::string address;     // where to connect: "169.254.192.51" (no mDNS lookup needed)
    std::string user;        // "" = guest
    std::string password;
};

struct Listed {
    bool                     ok = false;
    bool                     denied = false;   // wrong user name or password
    std::vector<std::string> volumes;
    std::string              says;
};
Listed list_volumes(const Login& login);       // blocking: connect, list, disconnect

struct Done {
    bool        ok = false;
    bool        denied = false;
    std::string says;
};
Done mount(const Login& login, const std::string& volume, const std::string& point);   // blocking, <= 90 s
Done unmount(const std::string& point);                                                 // blocking

}  // namespace netscope::netafp
