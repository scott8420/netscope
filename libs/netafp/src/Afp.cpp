// netafp -- AFP through Netatalk Client (milestone 10). See Afp.hpp.
#include "netafp/Afp.hpp"

#ifdef NETAFP_HAVE_AFPSL
extern "C" {
#include <afp.h>
#include <afpsl.h>
}
#endif

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace netscope::netafp {

// ── Pure ────────────────────────────────────────────────────────────────────

namespace {
std::string pct_encode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); out += b; }
    }
    return out;
}
std::string pct_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else out += s[i];
    }
    return out;
}
// mountinfo escapes space, tab, newline, backslash as \ooo.
std::string octal_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() &&
            std::isdigit((unsigned char)s[i + 1]) && std::isdigit((unsigned char)s[i + 2]) && std::isdigit((unsigned char)s[i + 3])) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 3), nullptr, 8));
            i += 3;
        } else out += s[i];
    }
    return out;
}
}  // namespace

std::string volume_uri(const std::string& host, const std::string& volume) {
    return "afp://" + host + "/" + pct_encode(volume) + "/";
}

bool is_afp_uri(const std::string& uri) { return uri.rfind("afp://", 0) == 0; }

bool split_uri(const std::string& uri, std::string& host, std::string& volume) {
    if (!is_afp_uri(uri)) return false;
    std::string rest = uri.substr(6);
    const auto slash = rest.find('/');
    if (slash == std::string::npos || slash == 0) return false;
    host = rest.substr(0, slash);
    std::string v = rest.substr(slash + 1);
    while (!v.empty() && v.back() == '/') v.pop_back();
    if (v.empty() || v.find('/') != std::string::npos) return false;
    volume = pct_decode(v);
    return true;
}

std::string short_host(const std::string& host) {
    const std::string suffix = ".local";
    if (host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0)
        return host.substr(0, host.size() - suffix.size());
    return host;
}

std::string mountpoint_for(const std::string& home, const std::string& host, const std::string& volume) {
    std::string v = volume;
    for (auto& c : v) if (c == '/') c = '-';
    std::string h = short_host(host);
    for (auto& c : h) if (c == '/') c = '-';
    return home + "/" + h + "/" + v;
}

std::vector<Mount> parse_mountinfo(const std::string& text) {
    // id parent major:minor root mountpoint options [optional...] - fstype source superopts
    std::vector<Mount> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::vector<std::string> f;
        std::string w;
        while (ls >> w) f.push_back(w);
        size_t dash = 0;
        for (size_t i = 6; i < f.size(); ++i) if (f[i] == "-") { dash = i; break; }
        if (dash == 0 || dash + 2 >= f.size() || f.size() < 5) continue;
        const std::string& type = f[dash + 1];
        if (type != "fuse.afpfs" && type != "fuse.afpfsd" && type != "fuse.afp") continue;
        out.push_back({octal_decode(f[4]), octal_decode(f[dash + 2])});
    }
    return out;
}

// ── System ──────────────────────────────────────────────────────────────────

namespace {

std::string find_tool(const char* name) {
    std::string path = std::getenv("PATH") ? std::getenv("PATH") : "";
    path += ":/usr/local/bin:/usr/bin";
    std::istringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) continue;
        const std::string p = dir + "/" + name;
        if (::access(p.c_str(), X_OK) == 0) return p;
    }
    return {};
}

// $XDG_RUNTIME_DIR/netscope/ssh-askpass: prints what arrives on fd 3.
std::string askpass_dir() {
    const char* rt = std::getenv("XDG_RUNTIME_DIR");
    std::string dir = std::string(rt && *rt ? rt : "/tmp") + "/netscope-askpass-" + std::to_string(::getuid());
    ::mkdir(dir.c_str(), 0700);
    ::chmod(dir.c_str(), 0700);
    const std::string script = dir + "/ssh-askpass";
    std::ofstream(script, std::ios::trunc) << "#!/bin/sh\n# NetScope: hands afp_client the password from a private pipe (fd 3).\nexec cat <&3\n";
    ::chmod(script.c_str(), 0700);
    return dir;
}

struct Ran {
    int         status = -1;    // exit code; -1 = didn't run / killed
    bool        timed_out = false;
    std::string output;         // stdout + stderr
};

// Runs argv; `secret` (if any) is readable once on the child's fd 3. stdin is
// /dev/null. Output goes to an unlinked temp file, not a pipe: afp_client
// starts afpfsd, which inherits stdout/stderr and outlives the call -- on a
// pipe nobody reads any more, its next write would kill it (SIGPIPE) and the
// mount with it. Waits for the child itself, then reads the file.
Ran run(const std::vector<std::string>& argv, const std::string* secret, const std::string& path_front,
        std::chrono::seconds timeout) {
    Ran r;
    int sec[2] = {-1, -1};
    const int out = ::open("/tmp", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
    if (out < 0) { r.output = std::strerror(errno); return r; }
    if (secret && ::pipe2(sec, O_CLOEXEC) != 0) { r.output = std::strerror(errno); ::close(out); return r; }
    const pid_t pid = ::fork();
    if (pid < 0) { r.output = std::strerror(errno); return r; }
    if (pid == 0) {
        const int null = ::open("/dev/null", O_RDONLY);
        ::dup2(null, 0);
        ::dup2(out, 1);
        ::dup2(out, 2);
        if (secret) ::dup2(sec[0], 3);   // dup2 clears CLOEXEC on the copy
        if (!path_front.empty()) {
            std::string p = path_front + ":" + (std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin");
            ::setenv("PATH", p.c_str(), 1);
        }
        ::unsetenv("DISPLAY");           // afp_client must use our askpass, never a graphical one
        ::unsetenv("SSH_ASKPASS");
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        ::execv(args[0], args.data());
        std::_Exit(127);
    }
    if (secret) {
        ::close(sec[0]);
        const char* p = secret->data();
        size_t left = secret->size();
        while (left > 0) {
            const ssize_t n = ::write(sec[1], p, left);
            if (n <= 0) break;
            p += n;
            left -= static_cast<size_t>(n);
        }
        ::close(sec[1]);
    }
    const auto until = std::chrono::steady_clock::now() + timeout;
    int st = 0;
    for (;;) {
        const pid_t w = ::waitpid(pid, &st, WNOHANG);
        if (w == pid) { r.status = WIFEXITED(st) ? WEXITSTATUS(st) : -1; break; }
        if (std::chrono::steady_clock::now() >= until) {
            ::kill(pid, SIGTERM);
            ::waitpid(pid, &st, 0);
            r.timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    char buf[1024];
    ssize_t n;
    off_t at = 0;
    while ((n = ::pread(out, buf, sizeof buf, at)) > 0) { r.output.append(buf, static_cast<size_t>(n)); at += n; }
    ::close(out);
    return r;
}

// The last line afp_client printed that says something ("Could not mount volume Data").
std::string last_words(const std::string& out) {
    std::istringstream in(out);
    std::string line, best;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line.rfind("*******", 0) == 0) continue;
        best = line;
    }
    return best;
}

bool says_denied(const std::string& out) {
    return out.find("Login error") != std::string::npos || out.find("ermission denied") != std::string::npos ||
           out.find("authentication") != std::string::npos || out.find("Authentication") != std::string::npos;
}

}  // namespace

bool available() { return missing().empty(); }

std::string missing() {
#ifndef NETAFP_HAVE_AFPSL
    return "Netatalk Client's library (libafpsl) wasn't found when NetScope was built, so AFP is off.";
#else
    if (find_tool("afp_client").empty())
        return "Netatalk Client's afp_client isn't installed (looked in PATH and /usr/local/bin), so AFP is off.";
    return {};
#endif
}

std::vector<Mount> mounts() {
    std::ifstream in("/proc/self/mountinfo");
    std::stringstream ss;
    ss << in.rdbuf();
    return parse_mountinfo(ss.str());
}

bool is_mounted(const std::string& point) {
    for (const auto& m : mounts()) if (m.point == point) return true;
    return false;
}

Listed list_volumes(const Login& login) {
    Listed r;
#ifndef NETAFP_HAVE_AFPSL
    r.says = missing();
#else
    afp_url url;
    afp_default_url(&url);
    std::snprintf(url.servername, sizeof url.servername, "%s", login.address.c_str());
    std::snprintf(url.username, sizeof url.username, "%s", login.user.c_str());
    std::snprintf(url.password, sizeof url.password, "%s", login.password.c_str());
    serverid_t id = nullptr;
    char msg[AFP_LOGINMESG_LEN + 1] = {};
    const int c = afp_sl_connect(&url, default_uams_mask(), &id, msg);
    explicit_bzero(url.password, sizeof url.password);
    if (c != 0) {
        r.denied = c == -EACCES;
        r.says = r.denied ? "The server turned down that user name and password."
                          : "Couldn't connect to " + login.address + ": " + std::strerror(-c) + ".";
        return r;
    }
    afp_volume_summary vols[64];
    unsigned int n = 0;
    const int g = afp_sl_getvols(id, &url, 0, 64, &n, vols);
    if (g != 0) {
        r.says = std::string("Logged in, but the volume list failed: ") + std::strerror(-g) + ".";
    } else {
        r.ok = true;
        for (unsigned int i = 0; i < n; ++i) r.volumes.emplace_back(vols[i].volume_name_printable);
        r.says = n == 0 ? "No volumes offered." : std::to_string(n) + (n == 1 ? " volume." : " volumes.");
    }
    afp_sl_disconnect(&id);
#endif
    return r;
}

Done mount(const Login& login, const std::string& volume, const std::string& point) {
    Done d;
    if (!available()) { d.says = missing(); return d; }
    if (is_mounted(point)) { d.ok = true; d.says = "Already mounted at " + point + "."; return d; }
    std::error_code ec;
    fs::create_directories(point, ec);
    if (ec) { d.says = "Couldn't make " + point + ": " + ec.message() + "."; return d; }
    if (!fs::is_empty(point, ec)) { d.says = point + " isn't empty; NetScope won't mount over files."; return d; }

    std::vector<std::string> argv{find_tool("afp_client"), "mount"};
    if (!login.user.empty()) { argv.push_back("-u"); argv.push_back(login.user); argv.push_back("-p"); argv.push_back("-"); }
    argv.push_back(login.address + ":" + volume);
    argv.push_back(point);
    const Ran r = run(argv, login.user.empty() ? nullptr : &login.password,
                      login.user.empty() ? std::string{} : askpass_dir(), std::chrono::seconds(90));
    // afp_client returns once afpfsd has the mount (or gave up). Trust the
    // kernel's table, not the exit code.
    for (int i = 0; i < 20 && !is_mounted(point); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (is_mounted(point)) { d.ok = true; d.says = "Mounted at " + point + "."; return d; }
    fs::remove(point, ec);   // leave no empty folder behind (only removes it if empty)
    d.denied = says_denied(r.output);
    if (r.timed_out) d.says = "The server took longer than 90 seconds to answer.";
    else if (d.denied) d.says = "The server turned down that user name and password.";
    else {
        const std::string w = last_words(r.output);
        d.says = w.empty() ? "afp_client gave up (exit " + std::to_string(r.status) + ")." : w + ".";
    }
    return d;
}

Done unmount(const std::string& point) {
    Done d;
    if (!is_mounted(point)) { d.ok = true; d.says = "Not mounted."; return d; }
    std::string out;
    if (const auto tool = find_tool("afp_client"); !tool.empty()) {
        const Ran r = run({tool, "unmount", point}, nullptr, {}, std::chrono::seconds(30));
        out = last_words(r.output);
    }
    if (is_mounted(point))
        for (const char* t : {"fusermount3", "fusermount"})
            if (const auto tool = find_tool(t); !tool.empty()) {
                const Ran r = run({tool, "-u", point}, nullptr, {}, std::chrono::seconds(30));
                if (!is_mounted(point)) break;
                out = last_words(r.output);
            }
    if (is_mounted(point)) { d.says = out.empty() ? "It's still mounted." : out + "."; return d; }
    std::error_code ec;
    fs::remove(point, ec);
    d.ok = true;
    d.says = "Unmounted.";
    return d;
}

}  // namespace netscope::netafp
