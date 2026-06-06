#pragma once

#include <atomic>

#include "audiopipe/config.h"
#include "audiopipe/log.h"

namespace audiopipe {
namespace win {

// Capture -> packetize (§7) -> UDP send loop. Runs until `stop` is set.
// Tolerates the capture device or network being temporarily unavailable by
// retrying rather than crashing (spec §5.7). Returns 0 on clean stop.
// WSAStartup must already have been called.
int run_sender(const SenderConfig& cfg, std::atomic<bool>& stop, Logger& log);

} // namespace win
} // namespace audiopipe
