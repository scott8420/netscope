#pragma once
// netdiscover::SsdpSource -- UPnP devices as a core::Source (milestone 13).
//
// Plain POSIX sockets on the source's own thread (no new dependency). One UDP
// socket bound to port 1900 (SO_REUSEADDR: shared with any other SSDP
// listener) joined to 239.255.255.250 on every IPv4 interface that is up,
// multicast-capable and not loopback. It sends an M-SEARCH out of each of
// those interfaces at start, again after 3 s (UDP gets lost), then every 5
// min; it also hears NOTIFY alive / byebye. A new device's description is
// fetched over HTTP (3 s limit) on the same thread. Devices whose max-age
// lapses are Lost. Trouble arrives as a FeedEvent::Note.
//
// netdiscover::NetworkSource runs it beside AvahiSource through a Merger:
// that is what the app uses.
#include "core/Feed.hpp"

#include <memory>

namespace netscope::netdiscover {

class SsdpSource : public core::Source {
public:
    SsdpSource();
    ~SsdpSource() override;
    void start(core::Inbox& inbox) override;
    void stop() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// Avahi + SSDP, merged (Merge.hpp) into one Inbox.
class NetworkSource : public core::Source {
public:
    NetworkSource();
    ~NetworkSource() override;
    void start(core::Inbox& inbox) override;
    void stop() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}  // namespace netscope::netdiscover
