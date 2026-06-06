#pragma once

#include <cstddef>

namespace audiopipe {

// Clock-drift correction (spec §8.4, REQUIRED). The PC and Mac sample clocks
// differ slightly, so the receiver buffer slowly drifts toward under- or
// over-run. This controller watches the ring-buffer fill level and emits a
// tiny resampling ratio (bounded to ±0.1%) that nudges the buffer back to its
// target — smoothly, with no audible pitch steps. The ratio feeds Resampler.
//
// TODO(M2): implement the PI controller and validate over an accelerated
// multi-hour run (buffer stays centered, no under/overrun).
class DriftController {
public:
    DriftController(std::size_t target_fill, std::size_t capacity);

    // Call once per control tick with the current fill level. Returns the
    // resampling ratio to apply (≈1.0, clamped to [0.999, 1.001]).
    double update(std::size_t current_fill);

private:
    std::size_t target_;
    std::size_t capacity_;
    double      ratio_ = 1.0;
};

} // namespace audiopipe
