#pragma once

#include <cstddef>
#include <cstdint>

namespace audiopipe {

// Fixed audio format (spec §4): 32-bit float, 48 kHz, stereo.
inline constexpr std::uint32_t kSampleRate     = 48'000;
inline constexpr std::uint16_t kChannels       = 2;
inline constexpr std::uint16_t kBytesPerSample = 4; // IEEE float32

// Frame sizing (spec §4). Default 96 samples/channel == 2 ms @ 48 kHz.
inline constexpr std::uint16_t kDefaultFrameSamples = 96;
inline constexpr std::uint16_t kMinFrameSamples     = 48;
inline constexpr std::uint16_t kMaxFrameSamples     = 240;

// Wire protocol (spec §7). Bytes 'A','P','L','1' read as a little-endian
// uint32; the trailing '1' encodes the protocol version.
inline constexpr std::uint32_t kMagic           = 0x314C5041u;
inline constexpr std::uint8_t  kProtocolVersion = 1;

enum class FormatId : std::uint8_t {
    Float32 = 0,
};

// Exact on-the-wire packet header (spec §7). Serialized little-endian; both
// ends are little-endian x86/ARM (LAN, personal use). Payload of interleaved
// float32 samples follows the header.
#pragma pack(push, 1)
struct PacketHeader {
    std::uint32_t magic;       // kMagic (encodes version)
    std::uint16_t sequence;    // monotonic, wraps
    std::uint64_t timestamp;   // sender sample-clock count
    std::uint16_t frame_count; // samples per channel in this packet
    std::uint8_t  channels;    // == kChannels
    std::uint8_t  format_id;   // FormatId
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 18, "PacketHeader must be 18 bytes on the wire");

// Size in bytes of the interleaved float32 payload for `frame_count` samples
// per channel.
constexpr std::size_t payload_bytes(std::uint16_t frame_count,
                                    std::uint16_t channels = kChannels) {
    return static_cast<std::size_t>(frame_count) * channels * kBytesPerSample;
}

} // namespace audiopipe
