#include "Log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <array>
#include <string>

namespace netscope::log {

namespace {
// The last Area plus one. A new area goes at the end of the enum AND here;
// get() returns null past the table rather than reading off its end.
constexpr int kAreaCount = static_cast<int>(Area::Mounts) + 1;

const char* area_name(Area a) {
    switch (a) {
        case Area::App:      return "app";
        case Area::Shell:    return "shell";
        case Area::Registry: return "registry";
        case Area::Recents:  return "recents";
        case Area::Io:       return "io";
        case Area::Devices:  return "devices";
        case Area::Mounts:   return "mounts";
    }
    return "?";
}

std::array<std::shared_ptr<spdlog::logger>, kAreaCount>& loggers() {
    static std::array<std::shared_ptr<spdlog::logger>, kAreaCount> a{};
    return a;
}
}  // namespace

void init() {
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto& a = loggers();
    for (int i = 0; i < kAreaCount; ++i) {
        const auto area = static_cast<Area>(i);
        auto lg = std::make_shared<spdlog::logger>(area_name(area), sink);
        lg->set_level(spdlog::level::info);   // default INFO; raise one area to TRACE to debug
        a[static_cast<std::size_t>(i)] = lg;
    }
}

std::shared_ptr<spdlog::logger> get(Area area) {
    const auto i = static_cast<std::size_t>(area);
    return i < loggers().size() ? loggers()[i] : nullptr;
}

void set_level(Area area, spdlog::level::level_enum level) {
    if (auto lg = get(area)) lg->set_level(level);
}

}  // namespace netscope::log
