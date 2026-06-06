#include "audiopipe/drift_control.h"

namespace audiopipe {
namespace {
// Defaults tuned for a critically-damped, slow (multi-second) response — see
// tools/drift_sim. Error is normalized to [-1, 1] by capacity before these
// apply, so they are independent of the absolute buffer size.
constexpr double kDefaultKp     = 0.30;
constexpr double kDefaultKi     = 3.0e-4;
constexpr double kDefaultMaxDev = 0.001; // ±0.1% (spec §8.4)
} // namespace

DriftController::DriftController(std::size_t target_fill, std::size_t capacity)
    : target_(target_fill),
      capacity_(capacity == 0 ? 1 : capacity),
      kp_(kDefaultKp),
      ki_(kDefaultKi),
      max_dev_(kDefaultMaxDev) {}

void DriftController::set_gains(double kp, double ki, double max_dev) {
    kp_      = kp;
    ki_      = ki;
    max_dev_ = max_dev;
}

void DriftController::reset() {
    integral_ = 0.0;
    ratio_    = 1.0;
}

double DriftController::update(std::size_t current_fill) {
    // Positive error == buffer fuller than target == produce fewer samples.
    const double error = (static_cast<double>(current_fill) - static_cast<double>(target_)) /
                         static_cast<double>(capacity_);
    integral_ += error;

    // Anti-windup: keep the integral's contribution within the clamp range.
    if (ki_ > 0.0) {
        const double int_limit = (2.0 * max_dev_) / ki_;
        if (integral_ > int_limit) {
            integral_ = int_limit;
        } else if (integral_ < -int_limit) {
            integral_ = -int_limit;
        }
    }

    double correction = kp_ * error + ki_ * integral_;
    if (correction > max_dev_) {
        correction = max_dev_;
    } else if (correction < -max_dev_) {
        correction = -max_dev_;
    }

    ratio_ = 1.0 - correction;
    return ratio_;
}

} // namespace audiopipe
