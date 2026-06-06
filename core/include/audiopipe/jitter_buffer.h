#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace audiopipe {

// Fixed-depth jitter buffer (spec §8.1–8.3). Packets arrive keyed by sequence
// number (possibly reordered or missing); frames are popped in order at a
// steady playout rate. Behavior:
//   - Priming (§8.1): holds playout until `depth` frames are buffered, giving
//     a fixed depth-frame cushion against jitter.
//   - Concealment (§8.2): a missing sequence is filled by repeating the last
//     good frame (or silence if none yet); `lost_count()` tracks these.
//   - Late drop (§8.3): a packet older than the current playout position is
//     discarded; `dropped_late_count()` tracks these.
class JitterBuffer {
public:
    JitterBuffer(std::uint16_t depth_frames,
                 std::uint16_t frame_samples,
                 std::uint16_t channels);

    // Insert one packet's audio, keyed by sequence number. `sample_count` is
    // the number of interleaved floats; extra is ignored, short is zero-padded.
    void insert(std::uint16_t sequence, const float* samples, std::size_t sample_count);

    // Pop the next frame for playout into `out` (frame_samples * channels
    // floats). Returns true if a real frame was delivered, false if the output
    // was concealed or priming silence.
    bool pop_frame(float* out);

    std::uint64_t lost_count() const { return lost_; }
    std::uint64_t dropped_late_count() const { return dropped_late_; }
    std::uint64_t dropped_overflow_count() const { return dropped_overflow_; }
    std::size_t   frame_floats() const { return frame_floats_; }

private:
    std::size_t slot_index(std::uint16_t seq) const { return seq % window_; }
    float*       slot_ptr(std::size_t idx) { return &slots_[idx * frame_floats_]; }

    std::uint16_t depth_;
    std::size_t   frame_floats_;
    std::size_t   window_; // number of slots (>= depth, absorbs reorder)

    std::vector<float>        slots_;      // window_ * frame_floats_
    std::vector<std::uint16_t> slot_seq_;  // sequence held by each slot
    std::vector<std::uint8_t>  slot_valid_; // 1 if slot holds a pending frame
    std::vector<float>        last_good_;  // frame_floats_

    bool          started_       = false; // seen the first packet
    bool          primed_        = false; // priming cushion reached
    bool          have_last_good_ = false;
    std::uint16_t playout_seq_   = 0;     // next sequence to play
    std::size_t   pending_       = 0;     // valid slots not yet played

    std::uint64_t lost_             = 0;
    std::uint64_t dropped_late_     = 0;
    std::uint64_t dropped_overflow_ = 0;
};

} // namespace audiopipe
