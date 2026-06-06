#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace audiopipe {
namespace tools {

// Minimal WAV container for the loopback harness. Samples are interleaved
// 32-bit float in [-1, 1].
struct WavData {
    std::uint32_t      sample_rate = 48000;
    std::uint16_t      channels    = 2;
    std::vector<float> samples; // interleaved
};

// Write a 32-bit IEEE-float WAV. Returns false on I/O error.
bool write_wav_f32(const std::string& path, const WavData& w);

// Read a WAV. Supports 16-bit PCM and 32-bit IEEE float; converts to float.
// Returns false and sets `err` on failure or unsupported format.
bool read_wav(const std::string& path, WavData& out, std::string& err);

} // namespace tools
} // namespace audiopipe
