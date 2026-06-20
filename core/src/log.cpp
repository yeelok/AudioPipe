#include "audiopipe/log.h"

#include <chrono>
#include <cstdio>   // std::rename, std::remove
#include <ctime>
#include <iomanip>

namespace audiopipe {

Logger::Logger(const std::string& path, std::size_t max_bytes, int keep)
    : path_(path), max_bytes_(max_bytes), keep_(keep) {
    if (path_.empty()) {
        return;
    }
    // Seed the running size from any existing file (we open in append mode).
    {
        std::ifstream existing(path_, std::ios::ate | std::ios::binary);
        size_ = existing ? static_cast<std::size_t>(existing.tellg()) : 0;
    }
    out_.open(path_, std::ios::app);
}

// Close the active file, shift <path>.N-1 -> <path>.N (dropping the oldest) and
// <path> -> <path>.1, then reopen a fresh active file. Caller holds mu_.
void Logger::rotate() {
    out_.close();
    std::remove((path_ + "." + std::to_string(keep_)).c_str());
    for (int i = keep_ - 1; i >= 1; --i) {
        std::rename((path_ + "." + std::to_string(i)).c_str(),
                    (path_ + "." + std::to_string(i + 1)).c_str());
    }
    std::rename(path_.c_str(), (path_ + ".1").c_str());
    out_.open(path_, std::ios::trunc);
    size_ = 0;
}

void Logger::info(std::string_view msg) {
    if (!out_.is_open()) {
        return;
    }

    using clock = std::chrono::system_clock;
    const std::time_t t = clock::to_time_t(clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    // "YYYY-MM-DD HH:MM:SS  " (21) + msg + '\n'.
    const std::size_t line = 22 + msg.size();

    std::lock_guard<std::mutex> lock(mu_);
    if (max_bytes_ > 0 && keep_ > 0 && size_ + line > max_bytes_) {
        rotate();
        if (!out_.is_open()) {
            return; // couldn't reopen after rotation
        }
    }
    out_ << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "  " << msg << '\n';
    out_.flush();
    size_ = static_cast<std::size_t>(out_.tellp()); // exact running size
}

} // namespace audiopipe
