#pragma once

#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace audiopipe {

// Minimal timestamped file logger for startup/shutdown, device state, packet
// loss counts, drift ratio, and under/overruns (spec §10).
//
// This is for control-path events, NOT the realtime audio callback. A
// non-allocating / lock-free path for hot-loop counters arrives in M6.
class Logger {
public:
    // If `path` is empty, every log call is a no-op.
    explicit Logger(const std::string& path);

    void info(std::string_view msg);

private:
    std::ofstream out_;
    std::mutex    mu_;
};

} // namespace audiopipe
