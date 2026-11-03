// netfix_probe -- the Fix button with no window.
//
//   netfix_probe status [iface]   what NM says about the connection
//   netfix_probe on     [iface]   ipv4.link-local = enabled, applied, wait for 169.254
//   netfix_probe off    [iface]   back to the default (the undo)
//
// No iface = the primary connection (the one with the default route).
#include "netfix/LinkLocal.hpp"

#include <iostream>

int main(int argc, char** argv) {
    namespace nf = netscope::netfix;
    const std::string verb  = argc > 1 ? argv[1] : "";
    const std::string iface = argc > 2 ? argv[2] : "";
    if (verb != "status" && verb != "on" && verb != "off") {
        std::cerr << "usage: netfix_probe status|on|off [iface]\n";
        return 2;
    }
    const nf::Result r = verb == "status" ? nf::find(iface) : nf::set_link_local(iface, verb == "on");
    std::cout << r.says << "\n";
    if (!r.target.conn_path.empty())
        std::cout << "  NM " << r.target.nm_version << ", " << r.target.iface << ", connection "
                  << r.target.conn_path << ", ipv4.method " << r.target.method << ", ipv4.link-local "
                  << r.target.link_local << "\n";
    return r.ok ? 0 : 1;
}
