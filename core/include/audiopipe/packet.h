#pragma once

#include <cstddef>
#include <cstdint>

#include "audiopipe/format.h"

namespace audiopipe {

// Wire (de)serialization for the §7 packet.
//
// encode(): writes the header (little-endian) followed by `sample_count`
// interleaved float32 samples into `dst` (capacity `dst_cap`). Returns the
// total bytes written, or 0 if the buffer is too small.
//
// decode_header(): validates magic/version/format and fills `out`. Returns
// false if `len` is too small or the header is invalid.
//
// TODO(M1): implement in core/src/packet.cpp with round-trip, sequence-wrap,
// and endianness unit tests.
std::size_t encode(const PacketHeader& hdr,
                   const float*        samples,
                   std::size_t         sample_count,
                   std::uint8_t*       dst,
                   std::size_t         dst_cap);

bool decode_header(const std::uint8_t* src, std::size_t len, PacketHeader& out);

} // namespace audiopipe
