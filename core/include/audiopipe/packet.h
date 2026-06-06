#pragma once

#include <cstddef>
#include <cstdint>

#include "audiopipe/format.h"

namespace audiopipe {

// Wire (de)serialization for the §7 packet. Integer fields are written
// little-endian explicitly (portable regardless of host byte order); float
// samples are serialized as their little-endian IEEE-754 bit pattern.

// Total bytes a packet with `sample_count` interleaved floats occupies.
constexpr std::size_t packet_size(std::size_t sample_count) {
    return sizeof(PacketHeader) + sample_count * kBytesPerSample;
}

// Write `hdr` followed by `sample_count` interleaved float32 samples into
// `dst` (capacity `dst_cap`). Returns the total bytes written, or 0 if the
// buffer is too small.
std::size_t encode(const PacketHeader& hdr,
                   const float*        samples,
                   std::size_t         sample_count,
                   std::uint8_t*       dst,
                   std::size_t         dst_cap);

// Validate magic/version/format and fill `out`. Returns false if `len` is
// smaller than the header or the header is invalid.
bool decode_header(const std::uint8_t* src, std::size_t len, PacketHeader& out);

// Copy the interleaved float payload into `out` (capacity `out_cap` floats).
// Call after decode_header. Returns the number of floats copied
// (== frame_count * channels), or 0 if `len` is too short for the declared
// payload or `out_cap` is too small.
std::size_t decode_payload(const std::uint8_t* src, std::size_t len,
                           const PacketHeader& hdr, float* out, std::size_t out_cap);

} // namespace audiopipe
