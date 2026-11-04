// netafp_probe -- AFP with no window.
//
//   netafp_probe volumes <address> [user]
//   netafp_probe mount   <address> <volume> [user]     -> ~/<address>/<volume>
//   netafp_probe unmount <mount point>
//   netafp_probe mounts
//
// The password is asked on the terminal (not echoed); NETAFP_PASS skips the
// prompt for scripts. No user = guest.
#include "netafp/Afp.hpp"

#include <cstdlib>
#include <iostream>
#include <unistd.h>

int main(int argc, char** argv) {
    namespace na = netscope::netafp;
    const std::string verb = argc > 1 ? argv[1] : "";
    auto login = [&](int user_at) {
        na::Login l;
        l.address = argc > 2 ? argv[2] : "";
        l.user = argc > user_at ? argv[user_at] : "";
        if (!l.user.empty()) {
            if (const char* p = std::getenv("NETAFP_PASS")) l.password = p;
            else if (const char* p = ::getpass("Password: ")) l.password = p;
        }
        return l;
    };
    if (verb == "mounts") {
        for (const auto& m : na::mounts()) std::cout << m.source << "  on  " << m.point << "\n";
        return 0;
    }
    if (!na::available()) { std::cerr << na::missing() << "\n"; return 1; }
    if (verb == "volumes" && argc > 2) {
        const auto r = na::list_volumes(login(3));
        std::cout << r.says << "\n";
        for (const auto& v : r.volumes) std::cout << "  " << v << "   " << na::volume_uri(argv[2], v) << "\n";
        return r.ok ? 0 : 1;
    }
    if (verb == "mount" && argc > 3) {
        const char* home = std::getenv("HOME");
        const auto point = na::mountpoint_for(home ? home : "/tmp", argv[2], argv[3]);
        const auto r = na::mount(login(4), argv[3], point);
        std::cout << r.says << "\n";
        return r.ok ? 0 : 1;
    }
    if (verb == "unmount" && argc > 2) {
        const auto r = na::unmount(argv[2]);
        std::cout << r.says << "\n";
        return r.ok ? 0 : 1;
    }
    std::cerr << "usage: netafp_probe volumes <address> [user] | mount <address> <volume> [user] |"
                 " unmount <mount point> | mounts\n";
    return 2;
}
