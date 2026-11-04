#pragma once
// core::Feed -- the seam discovery comes through (s002). GTK-free.
//
//   Source (any thread) --push--> Inbox --notify--> UI (main thread)
//                                   ^                  |
//                                   +------drain-------+--> apply() -> Network
//
// A Source runs on its own thread and only ever pushes events into the Inbox.
// The Inbox is the one place threads meet: a mutex-guarded queue, plus a
// notify hook the UI points at a Glib::Dispatcher (thread-safe to emit). The UI
// drains on the main thread and applies to the Network. Nothing else crosses.
// So the UI never waits on the network (REQUIREMENTS), and the core never
// learns what a main loop is.
//
// s002 ships FixtureSource -- canned devices on a timer, through the very same
// path. s003's libs/netdiscover (Avahi) is the second Source, and nothing
// above this file changes when it arrives.
#include "core/Devices.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace netscope::core {

struct FeedEvent {
    // Merged (milestone 13): this id is now part of another device (a UPnP
    // device folded into the Avahi host at its address) -- gone from the
    // list, not stale.
    enum class Kind { Seen, Lost, Note, Merged };
    Kind        kind = Kind::Seen;
    Sighting    sighting;    // Lost uses sighting.id only
    std::string note;        // Note: the source's own state, in plain words
                             // ("Avahi isn't running"). Not a device; apply() skips it.
};

class Inbox {
public:
    // Any thread. Queues the event, then calls notify (outside the lock).
    void push(FeedEvent e);
    // The UI's thread. Takes everything queued so far, oldest first.
    std::vector<FeedEvent> drain();
    // Set before any Source starts; called on the pushing thread.
    void set_notify(std::function<void()> f) { m_notify = std::move(f); }

private:
    std::mutex             m_mu;
    std::vector<FeedEvent> m_q;
    std::function<void()>  m_notify;
};

// Apply drained events to the table, in order. Returns how many changed it.
int apply(Network& net, const std::vector<FeedEvent>& events);

class Source {
public:
    virtual ~Source() = default;
    virtual void start(Inbox& inbox) = 0;   // returns at once; work runs on its own thread
    virtual void stop() = 0;                // joins; safe to call twice
};

// The fixture: what Scott's network looked like on Nov 3 (AFP_FINDINGS.md),
// delivered on a timer through the real seam. Facts we do not have are left
// blank, not invented.
std::vector<Sighting> fixture_sightings();

class FixtureSource : public Source {
public:
    // `step` is the pause between events (0 in the selftest).
    explicit FixtureSource(std::chrono::milliseconds step = std::chrono::milliseconds(250))
        : m_step(step) {}
    ~FixtureSource() override { stop(); }
    void start(Inbox& inbox) override;
    void stop() override;
    void finish();   // wait for the last event without cutting it short (selftest)

private:
    std::chrono::milliseconds   m_step;
    std::jthread                m_thread;
};

}  // namespace netscope::core
