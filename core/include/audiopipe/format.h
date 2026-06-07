#pragma once

#include <cstddef>
#include <cstdint>

namespace audiopipe {

// Fixed audio format (spec §4): 32-bit float, 48 kHz, stereo.
inline constexpr std::uint32_t kSampleRate     = 48'000;
inline constexpr std::uint16_t kChannels       = 2;
inline constexpr std::uint16_t kBytesPerSample = 4; // IEEE float32

// AOO stream block size (samples/channel per network packet). 96 == 2 ms @
// 48 kHz — the low-latency default we pass to AooFormatPcm_init / AooSource.
// (The wire protocol itself is now owned by AOO; see deps/aoo.)
inline constexpr std::uint16_t kDefaultFrameSamples = 96;
inline constexpr std::uint16_t kMinFrameSamples     = 48;
inline constexpr std::uint16_t kMaxFrameSamples     = 240;

} // namespace audiopipe
