// core::launch -- see Launch.hpp.
#include "core/Launch.hpp"

#include <algorithm>
#include <climits>

namespace netscope::core::launch {

namespace {
bool is_v6(const std::string& a) { return a.find(':') != std::string::npos; }
}  // namespace

std::string host_of(const Sighting& s, bool bracket) {
    if (!s.id.empty() && s.id.rfind("upnp-", 0) != 0) return s.id;
    for (const auto& a : s.addresses) if (!is_v6(a)) return a;
    for (const auto& a : s.addresses) return bracket ? "[" + a + "]" : a;
    return {};
}

std::string web_url(const Sighting& s) {
    const Service* best = nullptr;
    int best_rank = INT_MAX;
    for (const auto& v : s.services) {
        if (v.kind != "http" && v.kind != "https") continue;
        const bool tls = v.kind == "https";
        int rank = tls && v.port == 443 ? 0 : !tls && v.port == 80 ? 1 : 2;
        rank = rank * 200000 + (rank == 2 ? (tls ? 0 : 70000) + v.port : 0);   // other ports: https first, then lowest
        if (rank < best_rank) { best_rank = rank; best = &v; }
    }
    const auto host = host_of(s, true);
    if (!best || host.empty()) return {};
    const bool tls = best->kind == "https";
    const bool plain_port = best->port <= 0 || (tls ? best->port == 443 : best->port == 80);
    return std::string(tls ? "https://" : "http://") + host +
           (plain_port ? std::string{} : ":" + std::to_string(best->port)) + "/";
}

std::vector<std::string> ssh_args(const Sighting& s) {
    int port = 0;
    for (const auto& v : s.services)
        if (v.kind == "ssh" || v.kind == "sftp") { port = v.port; if (v.kind == "ssh") break; }
    const auto host = host_of(s, false);
    if (host.empty() || !std::any_of(s.services.begin(), s.services.end(),
                                     [](const Service& v) { return v.kind == "ssh" || v.kind == "sftp"; }))
        return {};
    if (port > 0 && port != 22) return {"-p", std::to_string(port), host};
    return {host};
}

const std::vector<std::string>& terminals() {
    static const std::vector<std::string> t{"ptyxis", "gnome-terminal", "kgx", "xdg-terminal-exec", "konsole", "xterm"};
    return t;
}

std::vector<std::string> terminal_argv(const std::string& terminal, const std::vector<std::string>& args) {
    // sh keeps a failed ssh on screen; the arguments travel as "$@", never as text.
    std::vector<std::string> cmd{"sh", "-c",
        "ssh \"$@\" || { echo; echo 'ssh ended with an error. Press Enter to close.'; read _; }", "ssh"};
    cmd.insert(cmd.end(), args.begin(), args.end());
    std::vector<std::string> out{terminal};
    if (terminal == "ptyxis")                                   { out.push_back("--new-window"); out.push_back("--"); }
    else if (terminal == "gnome-terminal" || terminal == "kgx") { out.push_back("--"); }
    else if (terminal == "konsole" || terminal == "xterm")      { out.push_back("-e"); }
    // xdg-terminal-exec takes the command as it is.
    out.insert(out.end(), cmd.begin(), cmd.end());
    return out;
}

}  // namespace netscope::core::launch
