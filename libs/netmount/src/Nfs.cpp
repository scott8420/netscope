// netmount::nfs -- see Nfs.hpp.
#include "netmount/Nfs.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstring>

namespace netscope::netmount::nfs {

namespace {

void put32(std::string& o, std::uint32_t v) {
    o += static_cast<char>((v >> 24) & 0xff);
    o += static_cast<char>((v >> 16) & 0xff);
    o += static_cast<char>((v >> 8) & 0xff);
    o += static_cast<char>(v & 0xff);
}

// XDR reader: big-endian words, opaque data padded to 4.
struct Xdr {
    const std::string& b;
    std::size_t at = 0;
    bool ok = true;
    std::uint32_t u32() {
        if (at + 4 > b.size()) { ok = false; return 0; }
        const auto* p = reinterpret_cast<const unsigned char*>(b.data() + at);
        at += 4;
        return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
    }
    std::string str(std::size_t max = 4096) {
        const auto n = u32();
        if (!ok || n > max || at + n > b.size()) { ok = false; return {}; }
        std::string s = b.substr(at, n);
        at += (n + 3) & ~std::size_t(3);
        if (at > b.size()) ok = false;
        return s;
    }
};

std::string call(std::uint32_t xid, std::uint32_t prog, std::uint32_t vers, std::uint32_t proc) {
    std::string o;
    put32(o, xid);
    put32(o, 0);       // CALL
    put32(o, 2);       // RPC version 2
    put32(o, prog);
    put32(o, vers);
    put32(o, proc);
    put32(o, 0); put32(o, 0);   // cred: AUTH_NULL
    put32(o, 0); put32(o, 0);   // verf: AUTH_NULL
    return o;
}

// Past the reply header to the results; false unless MSG_ACCEPTED + SUCCESS for xid.
bool accepted(Xdr& x, std::uint32_t xid) {
    if (x.u32() != xid || x.u32() != 1 /*REPLY*/ || x.u32() != 0 /*MSG_ACCEPTED*/) return false;
    x.u32();             // verf flavor
    x.str(400);          // verf body
    return x.ok && x.u32() == 0 /*SUCCESS*/ && x.ok;
}

std::atomic<std::uint32_t> g_xid{0x4e530000};   // "NS"

// ── the one place that touches the network ──────────────────────────────────

struct Fd {
    int fd = -1;
    ~Fd() { if (fd >= 0) ::close(fd); }
};

enum class Got { Ok, Refused, Timeout, Other };

Got connect_to(Fd& s, const std::string& address, int port, int timeout_ms) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo* res = nullptr;
    std::string a = address;
    if (a.size() > 2 && a.front() == '[' && a.back() == ']') a = a.substr(1, a.size() - 2);
    if (::getaddrinfo(a.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return Got::Other;
    s.fd = ::socket(res->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s.fd < 0) { ::freeaddrinfo(res); return Got::Other; }
    ::fcntl(s.fd, F_SETFL, ::fcntl(s.fd, F_GETFL) | O_NONBLOCK);
    int r = ::connect(s.fd, res->ai_addr, res->ai_addrlen);
    ::freeaddrinfo(res);
    if (r != 0 && errno != EINPROGRESS) return errno == ECONNREFUSED ? Got::Refused : Got::Other;
    if (r != 0) {
        pollfd p{s.fd, POLLOUT, 0};
        if (::poll(&p, 1, timeout_ms) <= 0) return Got::Timeout;
        int err = 0;
        socklen_t len = sizeof err;
        ::getsockopt(s.fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err) return err == ECONNREFUSED ? Got::Refused : Got::Other;
    }
    return Got::Ok;
}

// Send one record, read until the last fragment. nullopt on timeout / close.
std::optional<std::string> exchange(const std::string& address, int port, const std::string& body,
                                    int timeout_ms, Got& why) {
    Fd s;
    why = connect_to(s, address, port, timeout_ms);
    if (why != Got::Ok) return std::nullopt;
    const std::string out = record(body);
    std::size_t sent = 0;
    while (sent < out.size()) {
        pollfd p{s.fd, POLLOUT, 0};
        if (::poll(&p, 1, timeout_ms) <= 0) { why = Got::Timeout; return std::nullopt; }
        const auto n = ::send(s.fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) { why = Got::Other; return std::nullopt; }
        sent += static_cast<std::size_t>(n);
    }
    std::string in;
    char buf[4096];
    while (in.size() < (1u << 20)) {
        if (auto body_in = defragment(in)) return body_in;
        pollfd p{s.fd, POLLIN, 0};
        if (::poll(&p, 1, timeout_ms) <= 0) { why = Got::Timeout; return std::nullopt; }
        const auto n = ::recv(s.fd, buf, sizeof buf, 0);
        if (n <= 0) { why = Got::Other; return std::nullopt; }
        in.append(buf, static_cast<std::size_t>(n));
    }
    why = Got::Other;
    return std::nullopt;
}

}  // namespace

std::string record(const std::string& body) {
    std::string o;
    put32(o, 0x80000000u | static_cast<std::uint32_t>(body.size()));
    return o + body;
}

std::string call_getport(std::uint32_t xid, std::uint32_t prog, std::uint32_t vers, std::uint32_t prot) {
    std::string o = call(xid, kPortmap, 2, 3 /*PMAPPROC_GETPORT*/);
    put32(o, prog);
    put32(o, vers);
    put32(o, prot);
    put32(o, 0);
    return o;
}

std::string call_export(std::uint32_t xid, std::uint32_t vers) {
    return call(xid, kMountd, vers, 5 /*MOUNTPROC_EXPORT*/);
}

std::optional<std::uint32_t> parse_getport(const std::string& reply, std::uint32_t xid) {
    Xdr x{reply};
    if (!accepted(x, xid)) return std::nullopt;
    const auto port = x.u32();
    if (!x.ok || port > 65535) return std::nullopt;
    return port;
}

std::optional<std::vector<Export>> parse_exports(const std::string& reply, std::uint32_t xid) {
    Xdr x{reply};
    if (!accepted(x, xid)) return std::nullopt;
    std::vector<Export> out;
    while (x.ok && out.size() < 4096) {
        const auto more = x.u32();
        if (!x.ok) break;
        if (more == 0) return out;
        Export e;
        e.path = x.str(1024);
        while (x.ok) {
            const auto g = x.u32();
            if (!x.ok || g == 0) break;
            e.groups.push_back(x.str(255));
        }
        if (x.ok) out.push_back(std::move(e));
    }
    return std::nullopt;
}

std::optional<std::string> defragment(const std::string& stream) {
    std::string body;
    std::size_t at = 0;
    while (at + 4 <= stream.size()) {
        const auto* p = reinterpret_cast<const unsigned char*>(stream.data() + at);
        const std::uint32_t mark = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
                                   (std::uint32_t(p[2]) << 8) | p[3];
        const std::size_t len = mark & 0x7fffffffu;
        if (at + 4 + len > stream.size()) return std::nullopt;
        body.append(stream, at + 4, len);
        at += 4 + len;
        if (mark & 0x80000000u) return body;
    }
    return std::nullopt;
}

Listing list_exports(const std::string& address, const std::string& who, int timeout_ms) {
    Listing l;
    Got why = Got::Ok;
    // mountd v3, else v1 (same EXPORT answer); TCP only.
    std::uint32_t port = 0, vers = 0;
    for (std::uint32_t v : {3u, 1u}) {
        const auto xid = g_xid++;
        const auto r = exchange(address, 111, call_getport(xid, kMountd, v, kTcp), timeout_ms, why);
        if (!r) break;
        if (const auto p = parse_getport(*r, xid); p && *p) { port = *p; vers = v; break; }
    }
    if (!port) {
        if (why == Got::Refused)
            l.says = who + " has no portmapper (port 111 refused), so it can't list its NFS exports. "
                     "An NFSv4-only server lists nothing; mount it by path.";
        else if (why == Got::Timeout || why == Got::Other)
            l.says = who + " didn't answer on port 111 (the NFS portmapper).";
        else
            l.says = who + " runs no NFS mount service (mountd), so it can't list exports.";
        return l;
    }
    const auto xid = g_xid++;
    const auto r = exchange(address, static_cast<int>(port), call_export(xid, vers), timeout_ms, why);
    if (!r) {
        l.says = who + "'s NFS mount service (port " + std::to_string(port) + ") didn't answer.";
        return l;
    }
    const auto ex = parse_exports(*r, xid);
    if (!ex) {
        l.says = who + "'s NFS export list couldn't be read.";
        return l;
    }
    l.ok = true;
    l.exports = *ex;
    return l;
}

std::string export_uri(const std::string& host, const std::string& path) {
    static const char* hex = "0123456789ABCDEF";
    std::string out = "nfs://" + host + "/";
    for (unsigned char c : path) {
        if (c == '/') { if (out.back() != '/') out += '/'; continue; }
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    if (out.back() != '/') out += '/';
    return out;
}

std::string export_name(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    const auto slash = p.rfind('/');
    if (p.size() <= 1 || slash == std::string::npos) return p.empty() ? "/" : p;
    return p.substr(slash + 1);
}

}  // namespace netscope::netmount::nfs
