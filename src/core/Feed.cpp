#include "core/Feed.hpp"

namespace netscope::core {

void Inbox::push(FeedEvent e) {
    {
        std::lock_guard lock(m_mu);
        m_q.push_back(std::move(e));
    }
    if (m_notify) m_notify();
}

std::vector<FeedEvent> Inbox::drain() {
    std::lock_guard lock(m_mu);
    std::vector<FeedEvent> out;
    out.swap(m_q);
    return out;
}

int apply(Network& net, const std::vector<FeedEvent>& events) {
    int changed = 0;
    for (const auto& e : events) {
        if (e.kind == FeedEvent::Kind::Note) continue;
        const auto before = net.get(e.sighting.id);
        if (e.kind == FeedEvent::Kind::Seen) net.seen(e.sighting);
        else                                  net.lost(e.sighting.id);
        if (net.get(e.sighting.id) != before) ++changed;
    }
    return changed;
}

std::vector<Sighting> fixture_sightings() {
    // tc01: seen by Bonjour with a self-assigned address; AFP logged in by hand.
    // tc02: unplugged on Nov 3 -- it appears, then goes stale.
    // RASPI: named in ARC for s003/s005 (SMB share); its address is unknown here.
    return {
        {"tc01.local",  "tc01",  "Time Capsule", {"169.254.192.51"}, {{"afp", 548}}},
        {"raspi.local", "RASPI", "",             {},                 {{"smb", 445}, {"ssh", 22}}},
        {"tc02.local",  "tc02",  "Time Capsule", {},                 {{"afp", 548}}},
    };
}

void FixtureSource::start(Inbox& inbox) {
    stop();
    m_thread = std::jthread([this, &inbox](std::stop_token st) {
        std::mutex mu;
        std::condition_variable_any cv;
        // Wait one step, or until stop is asked. False means stop.
        const auto pause = [&]() {
            if (m_step.count() == 0) return !st.stop_requested();
            std::unique_lock lock(mu);
            return !cv.wait_for(lock, st, m_step, [] { return false; });
        };
        for (const auto& s : fixture_sightings()) {
            if (!pause()) return;
            inbox.push({FeedEvent::Kind::Seen, s, {}});
        }
        if (!pause()) return;
        inbox.push({FeedEvent::Kind::Lost, Sighting{"tc02.local", {}, {}, {}, {}}, {}});
    });
}

void FixtureSource::finish() {
    if (m_thread.joinable()) m_thread.join();
}

void FixtureSource::stop() {
    if (m_thread.joinable()) {
        m_thread.request_stop();
        m_thread.join();
    }
}

}  // namespace netscope::core
