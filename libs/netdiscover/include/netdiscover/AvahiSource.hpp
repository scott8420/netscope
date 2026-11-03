#pragma once
// netdiscover::AvahiSource -- the network's own core::Source (milestone 4).
//
// Avahi over D-Bus through GIO (no new build dependency: gtkmm already brings
// GIO). Everything runs on the source's own thread, under its OWN
// GMainContext, on a PRIVATE system-bus connection -- so it shares nothing
// with GTK's main loop or GTK's bus connection. The only thing that leaves the
// thread is a core::FeedEvent pushed into the Inbox.
//
// For each type in browsed_types(): ServiceBrowserNew; each ItemNew is
// resolved (ResolveService) and handed to the Aggregator; ItemRemove likewise.
// Trouble (no system bus, no avahi-daemon, a browser Failure) arrives as a
// FeedEvent::Note in plain words, so the UI can say why the list is empty.
#include "core/Feed.hpp"

#include <memory>

namespace netscope::netdiscover {

class AvahiSource : public core::Source {
public:
    AvahiSource();
    ~AvahiSource() override;
    void start(core::Inbox& inbox) override;
    void stop() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}  // namespace netscope::netdiscover
