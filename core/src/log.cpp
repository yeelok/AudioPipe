#include "audiopipe/log.h"

#include <chrono>
#include <ctime>
#include <iomanip>

namespace audiopipe {

Logger::Logger(const std::string& path) {
    if (!path.empty()) {
        out_.open(path, std::ios::app);
    }
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

    std::lock_guard<std::mutex> lock(mu_);
    out_ << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "  " << msg << '\n';
    out_.flush();
}

} // namespace audiopipe
