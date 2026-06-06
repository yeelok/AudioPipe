#include "audiopipe/jitter_buffer.h"

#include <algorithm>

namespace audiopipe {
namespace {

// True if sequence `a` is strictly before `b`, accounting for uint16 wrap.
bool seq_before(std::uint16_t a, std::uint16_t b) {
    return static_cast<std::int16_t>(a - b) < 0;
}

// Slots must comfortably exceed the priming depth and any realistic reorder
// window. A power-of-two keeps `seq % window_` cheap.
std::size_t pick_window(std::uint16_t depth) {
    std::size_t w = 512;
    while (w <= static_cast<std::size_t>(depth) * 2) {
        w *= 2;
    }
    return w;
}

} // namespace

JitterBuffer::JitterBuffer(std::uint16_t depth_frames, std::uint16_t frame_samples,
                           std::uint16_t channels)
    : depth_(depth_frames),
      frame_floats_(static_cast<std::size_t>(frame_samples) * channels),
      window_(pick_window(depth_frames)),
      slots_(window_ * frame_floats_, 0.0f),
      slot_seq_(window_, 0),
      slot_valid_(window_, 0),
      last_good_(frame_floats_, 0.0f) {}

void JitterBuffer::insert(std::uint16_t sequence, const float* samples,
                          std::size_t sample_count) {
    if (!started_) {
        started_     = true;
        playout_seq_ = sequence; // first arrival anchors the playout cursor
    } else if (seq_before(sequence, playout_seq_)) {
        if (primed_) {
            ++dropped_late_; // §8.3: older than playout position
            return;
        }
        playout_seq_ = sequence; // still priming — re-anchor to the earlier frame
    }

    const std::size_t idx = slot_index(sequence);
    if (slot_valid_[idx]) {
        if (slot_seq_[idx] == sequence) {
            return; // duplicate
        }
        ++dropped_overflow_; // slot collision (reorder beyond the window)
        return;
    }

    float* dst = slot_ptr(idx);
    const std::size_t n = std::min(sample_count, frame_floats_);
    std::copy(samples, samples + n, dst);
    if (n < frame_floats_) {
        std::fill(dst + n, dst + frame_floats_, 0.0f);
    }
    slot_seq_[idx]   = sequence;
    slot_valid_[idx] = 1;
    ++pending_;
}

bool JitterBuffer::pop_frame(float* out) {
    if (!started_) {
        std::fill(out, out + frame_floats_, 0.0f);
        return false;
    }

    if (!primed_) {
        if (pending_ >= depth_) {
            primed_ = true;
        } else {
            std::fill(out, out + frame_floats_, 0.0f); // priming silence
            return false;
        }
    }

    const std::size_t idx = slot_index(playout_seq_);
    if (slot_valid_[idx] && slot_seq_[idx] == playout_seq_) {
        const float* src = slot_ptr(idx);
        std::copy(src, src + frame_floats_, out);
        std::copy(src, src + frame_floats_, last_good_.begin());
        have_last_good_  = true;
        slot_valid_[idx] = 0;
        --pending_;
        ++playout_seq_;
        return true;
    }

    // §8.2: missing frame — repeat last good (or silence if none yet).
    if (have_last_good_) {
        std::copy(last_good_.begin(), last_good_.end(), out);
    } else {
        std::fill(out, out + frame_floats_, 0.0f);
    }
    ++lost_;
    ++playout_seq_;
    return false;
}

} // namespace audiopipe
