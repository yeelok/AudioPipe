#pragma once

#include <cstddef>
#include <cstdint>

#include "audiopipe/format.h"

namespace audiopipe {

// Fixed-depth jitter buffer (spec §8.1–8.3): orders received packets by
// sequence number, conceals a missing packet by repeating the last good frame
// (§8.2), and drops packets older than the current playout position (§8.3).
//
// TODO(M1): implement storage, playout cursor, last-good frame, loss counting.
class JitterBuffer {
public:
    JitterBuffer(std::uint16_t depth_frames,
                 std::uint16_t frame_samples,
                 std::uint16_t channels);

    // Insert one packet's audio, keyed by sequence number.
    void insert(std::uint16_t sequence, const float* samples, std::size_t sample_count);

    // Pop the next frame for playout into `out` (frame_samples * channels
    // floats). Returns false when it had to conceal (loss/underrun).
    bool pop_frame(float* out);

    std::uint64_t lost_count() const { return lost_; }

private:
    std::uint16_t depth_;
    std::uint16_t frame_samples_;
    std::uint16_t channels_;
    std::uint64_t lost_ = 0;
};

} // namespace audiopipe
