// netdiscover::SsdpSource and NetworkSource -- see SsdpSource.hpp.
#include "netdiscover/SsdpSource.hpp"
#include "netdiscover/AvahiSource.hpp"
#include "netdiscover/Merge.hpp"
#include "netdiscover/Ssdp.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace netscope::netdiscover {

namespace {
using Clock = std::chrono::steady_clock;

// IPv4 addresses of interfaces that are up, multicast-capable, not loopback.
std::vector<in_addr> multicast_ifaces() {
    std::vector<in_addr> out;
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return out;
    for (auto* i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_MULTICAST) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        out.push_back(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr);
    }
    freeifaddrs(ifs);
    return out;
}

int udp_socket(int port, bool reuse) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    const int one = 1;
    if (reuse) {
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    }
    const unsigned char ttl = 2;
    ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { ::close(fd); return -1; }
    return fd;
}

// GET url, plain HTTP/1.0, whole thing within `limit`. Body or "".
std::string http_get(const ssdp::Url& u, std::chrono::milliseconds limit) {
    const auto until = Clock::now() + limit;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (::getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res) != 0 || !res) return {};
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    std::string got;
    auto left = [&] { return static_cast<int>(std::max<long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count())); };
    if (fd >= 0) {
        ::connect(fd, res->ai_addr, res->ai_addrlen);
        pollfd p{fd, POLLOUT, 0};
        int err = 0;
        socklen_t el = sizeof err;
        if (::poll(&p, 1, left()) == 1 && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) {
            const std::string req = "GET " + u.path + " HTTP/1.0\r\nHost: " + u.host + ":" + std::to_string(u.port) +
                                    "\r\nUser-Agent: NetScope/1\r\nConnection: close\r\n\r\n";
            if (::send(fd, req.data(), req.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(req.size())) {
                char buf[4096];
                while (got.size() < 256 * 1024) {
                    pollfd q{fd, POLLIN, 0};
                    if (::poll(&q, 1, left()) != 1) break;
                    const auto n = ::recv(fd, buf, sizeof buf, 0);
                    if (n <= 0) break;
                    got.append(buf, static_cast<std::size_t>(n));
                }
            }
        }
        ::close(fd);
    }
    ::freeaddrinfo(res);
    const auto body = got.find("\r\n\r\n");
    if (got.rfind("HTTP/1.", 0) != 0 || got.find(" 200", 0) > got.find("\r\n") || body == std::string::npos) return {};
    return got.substr(body + 4);
}
}  // namespace

// ── SsdpSource ──────────────────────────────────────────────────────────────

struct SsdpSource::Impl {
    std::jthread thread;

    static void run(std::stop_token st, core::Inbox* inbox) {
        auto note = [&](std::string s) { inbox->push({core::FeedEvent::Kind::Note, {}, "SSDP: " + std::move(s)}); };
        const int search = udp_socket(0, false);       // our M-SEARCHes; answers come back here
        const int listen = udp_socket(ssdp::kPort, true);   // NOTIFY alive / byebye
        if (search < 0) { note("can't open a UDP socket -- UPnP devices won't be listed"); return; }
        if (listen < 0) note("port 1900 is taken; UPnP devices are found by searching only");

        in_addr group{};
        ::inet_pton(AF_INET, ssdp::kGroup, &group);
        std::vector<in_addr> joined;
        auto join_all = [&] {
            const auto ifs = multicast_ifaces();
            for (const auto& a : ifs) {
                bool have = false;
                for (const auto& j : joined) have = have || j.s_addr == a.s_addr;
                if (have || listen < 0) continue;
                ip_mreq m{group, a};
                if (::setsockopt(listen, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) == 0) joined.push_back(a);
            }
            return ifs;
        };
        auto send_search = [&] {
            const auto ifs = join_all();
            const std::string req = ssdp::search_request(2);
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(ssdp::kPort);
            to.sin_addr = group;
            for (const auto& a : ifs) {   // out of every interface: no default route needed
                ::setsockopt(search, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof a);
                ::sendto(search, req.data(), req.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
            }
            return ifs.size();
        };

        ssdp::Aggregator agg;
        auto push = [&](const std::vector<core::FeedEvent>& es) { for (const auto& e : es) inbox->push(e); };
        auto handle = [&](const std::string& dgram) {
            const auto r = ssdp::parse(dgram);
            if (!r) return;
            bool fetch = false;
            push(agg.reply(*r, Clock::now(), &fetch));
            if (!fetch) return;
            if (const auto u = ssdp::parse_url(r->location)) {
                const auto xml = http_get(*u, std::chrono::seconds(3));
                if (!xml.empty()) push(agg.described(ssdp::uuid_of(r->usn), ssdp::parse_description(xml)));
            }
        };

        if (send_search() == 0) note("no network interface to search on yet");
        auto next = Clock::now() + std::chrono::seconds(3);
        int sent = 1;
        char buf[2048];
        while (!st.stop_requested()) {
            pollfd p[2] = {{search, POLLIN, 0}, {listen, POLLIN, 0}};
            ::poll(p, listen >= 0 ? 2 : 1, 500);
            for (auto& q : p) {
                if (q.fd < 0 || !(q.revents & POLLIN)) continue;
                for (;;) {
                    const auto n = ::recv(q.fd, buf, sizeof buf - 1, 0);
                    if (n <= 0) break;
                    handle(std::string(buf, static_cast<std::size_t>(n)));
                }
            }
            const auto now = Clock::now();
            push(agg.expire(now));
            if (now >= next) {
                send_search();
                ++sent;
                next = now + (sent < 3 ? std::chrono::seconds(3) : std::chrono::minutes(5));
            }
        }
        ::close(search);
        if (listen >= 0) ::close(listen);
    }
};

SsdpSource::SsdpSource() : m(std::make_unique<Impl>()) {}
SsdpSource::~SsdpSource() { stop(); }
void SsdpSource::start(core::Inbox& inbox) {
    if (m->thread.joinable()) return;
    m->thread = std::jthread([&inbox](std::stop_token st) { Impl::run(st, &inbox); });
}
void SsdpSource::stop() {
    if (!m->thread.joinable()) return;
    m->thread.request_stop();
    m->thread.join();
}

// ── NetworkSource: Avahi + SSDP through the Merger ──────────────────────────

struct NetworkSource::Impl {
    AvahiSource  avahi;
    SsdpSource   ssdp;
    core::Inbox  from_avahi, from_ssdp;
    std::mutex   mu;
    Merger       merger;
    core::Inbox* out = nullptr;

    void pump(Merger::From from, core::Inbox& in) {
        std::lock_guard l(mu);
        for (const auto& e : in.drain())
            for (auto& m : merger.take(from, e)) out->push(std::move(m));
    }
};

NetworkSource::NetworkSource() : m(std::make_unique<Impl>()) {}
NetworkSource::~NetworkSource() { stop(); }
void NetworkSource::start(core::Inbox& inbox) {
    m->out = &inbox;
    m->from_avahi.set_notify([this] { m->pump(Merger::From::Avahi, m->from_avahi); });
    m->from_ssdp.set_notify([this] { m->pump(Merger::From::Ssdp, m->from_ssdp); });
    m->avahi.start(m->from_avahi);
    m->ssdp.start(m->from_ssdp);
}
void NetworkSource::stop() {
    m->ssdp.stop();
    m->avahi.stop();
}

}  // namespace netscope::netdiscover
