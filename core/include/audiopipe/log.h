#pragma once

#include <cstddef>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace audiopipe {

// Minimal timestamped file logger for startup/shutdown, device state, the
// receiver heartbeat (fill ratio / measured rate), AOO warnings, and
// under/overruns (spec §10).
//
// Housekeeping: the file is size-rotated so an always-on auto-start service can't
// grow it without bound. When a write would push the active file past
// `max_bytes`, it rotates: <path> -> <path>.1 -> ... -> <path>.<keep>, dropping
// the oldest. Total on-disk log is capped at ~max_bytes * (keep + 1). Pass
// max_bytes = 0 to disable rotation (unbounded).
//
// Control-path only, NOT the realtime audio callback.
class Logger {
public:
    // If `path` is empty, every log call is a no-op.
    explicit Logger(const std::string& path,
                    std::size_t max_bytes = 5 * 1024 * 1024, // 5 MB per file
                    int keep = 3);                            // 3 rotated backups

    void info(std::string_view msg);

private:
    void rotate();  // call with mu_ held

    std::string   path_;
    std::ofstream out_;
    std::mutex    mu_;
    std::size_t   max_bytes_;
    int           keep_;
    std::size_t   size_ = 0; // bytes in the active file
};

} // namespace audiopipe
