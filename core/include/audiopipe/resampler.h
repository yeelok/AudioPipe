#pragma once

#include <cstddef>

namespace audiopipe {

// High-quality variable-rate resampler (spec §3, §8.4): a thin wrapper over
// libsoxr in variable-rate mode. Used only for drift correction, so the live
// ratio hovers around 1.0 and is driven by DriftController.
//
// TODO(M2): wire up libsoxr (soxr_create with SOXR_VR, soxr_set_io_ratio).
class Resampler {
public:
    Resampler(double initial_io_ratio, int channels);
    ~Resampler();

    Resampler(const Resampler&)            = delete;
    Resampler& operator=(const Resampler&) = delete;

    // Consume `in_frames` of interleaved input and apply `ratio` (desired
    // output/input sample-rate ratio, ≈1.0). Emits up to `out_cap` frames
    // (samples per channel) into the interleaved buffer `out`, which must hold
    // `out_cap * channels` floats. Returns the number of output frames produced.
    std::size_t process(const float* in, std::size_t in_frames,
                        float* out, std::size_t out_cap, double ratio);

private:
    void* soxr_ = nullptr; // soxr_t handle (channel count is held inside soxr)
};

} // namespace audiopipe
