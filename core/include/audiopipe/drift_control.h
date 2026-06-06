#pragma once

#include <cstddef>

namespace audiopipe {

// Clock-drift correction (spec §8.4, REQUIRED). The PC and Mac sample clocks
// differ slightly, so the receiver buffer slowly drifts toward under- or
// over-run. This PI controller watches the ring-buffer fill level and emits a
// tiny rate ratio (bounded to ±0.1% by default) that nudges the buffer back to
// its target — smoothly, with no audible pitch steps. The ratio feeds
// Resampler (it is the desired output/input sample-rate ratio).
//
// Error is normalized by capacity, so the gains are dimensionless and
// independent of the absolute buffer size.
class DriftController {
public:
    DriftController(std::size_t target_fill, std::size_t capacity);

    // Call once per control tick with the current fill level. Returns the
    // output/input ratio to apply (≈1.0; <1 drains a too-full buffer).
    double update(std::size_t current_fill);

    // Override the default PI gains and clamp. `max_dev` is the maximum
    // fractional deviation from 1.0 (0.001 == ±0.1%, per spec §8.4).
    void set_gains(double kp, double ki, double max_dev);
    void reset();

    double ratio() const { return ratio_; }
    double integral() const { return integral_; }

private:
    std::size_t target_;
    std::size_t capacity_;
    double      kp_;
    double      ki_;
    double      max_dev_;
    double      integral_ = 0.0;
    double      ratio_    = 1.0;
};

} // namespace audiopipe
