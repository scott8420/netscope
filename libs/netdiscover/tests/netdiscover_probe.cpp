// netdiscover_probe [seconds] -- run NetworkSource (Avahi + SSDP, merged;
// milestone 13) with no window and print what it reports. For checking discovery on a real network: `./build/netdiscover_probe 8`.
#include "netdiscover/SsdpSource.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>

namespace core = netscope::core;

int main(int argc, char** argv) {
    const int secs = argc > 1 ? std::atoi(argv[1]) : 6;
    core::Inbox inbox;
    std::mutex mu;
    std::condition_variable cv;
    bool ready = false;
    inbox.set_notify([&] { { std::lock_guard l(mu); ready = true; } cv.notify_one(); });

    netscope::netdiscover::NetworkSource src;
    src.start(inbox);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(secs);
    for (;;) {
        std::unique_lock l(mu);
        if (!cv.wait_until(l, end, [&] { return ready; })) break;
        ready = false;
        l.unlock();
        for (const auto& e : inbox.drain()) {
            using K = core::FeedEvent::Kind;
            if (e.kind == K::Note) { std::cout << "note  " << e.note << "\n"; continue; }
            const auto& s = e.sighting;
            std::cout << (e.kind == K::Seen ? "seen  " : e.kind == K::Merged ? "fold  " : "lost  ") << s.id;
            if (e.kind == K::Seen) {
                std::cout << "  name=" << s.name << "  model=" << (s.model.empty() ? "-" : s.model) << "  addr=";
                for (const auto& a : s.addresses) std::cout << a << " ";
                std::cout << " svc=";
                for (const auto& v : s.services) std::cout << v.kind << ":" << v.port << " ";
            }
            std::cout << "\n";
        }
    }
    src.stop();
    std::cout << "done\n";
}
