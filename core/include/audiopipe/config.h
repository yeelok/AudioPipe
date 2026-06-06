#pragma once

#include <cstdint>
#include <string>

#include "audiopipe/format.h"

namespace audiopipe {

// Windows sender configuration (spec §5.5).
struct SenderConfig {
    std::string   dest_ip;                       // required
    std::uint16_t dest_port         = 0;         // required
    std::uint32_t sample_rate       = kSampleRate;
    std::uint16_t channels          = kChannels;
    std::uint16_t frame_samples     = kDefaultFrameSamples;
    std::uint16_t frames_per_packet = 1;         // spec §7 (start at 1)
    std::string   log_file;
};

// macOS receiver configuration (spec §6.5).
struct ReceiverConfig {
    std::uint16_t listen_port    = 0;            // required
    std::uint32_t sample_rate    = kSampleRate;
    std::uint16_t channels       = kChannels;
    std::uint16_t jitter_frames  = 3;            // spec §8.1 default
    std::string   device_name    = "AudioPipe";  // CoreAudio device to play into
    std::string   log_file;
};

// Parse JSON config from `path`. Throws std::runtime_error on a missing file,
// parse error, missing required field, or out-of-range frame size.
SenderConfig   load_sender_config(const std::string& path);
ReceiverConfig load_receiver_config(const std::string& path);

} // namespace audiopipe
