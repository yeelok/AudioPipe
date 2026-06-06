#include "audiopipe/resampler.h"

#include <stdexcept>
#include <string>

#include <soxr.h>

namespace audiopipe {

Resampler::Resampler(double initial_io_ratio, int channels) {
    // Variable-rate, high-quality, interleaved float32 in/out. The nominal
    // create rates are equal (base ratio 1.0); the live ratio is applied via
    // soxr_set_io_ratio below and on every process() call.
    soxr_io_spec_t      io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    soxr_quality_spec_t q  = soxr_quality_spec(SOXR_HQ, SOXR_VR);

    soxr_error_t err = nullptr;
    soxr_t       s = soxr_create(48000.0, 48000.0, static_cast<unsigned>(channels), &err, &io,
                                 &q, nullptr);
    if (s == nullptr || err != nullptr) {
        throw std::runtime_error(std::string("soxr_create failed: ") + (err ? err : "unknown"));
    }
    soxr_ = s;
    // soxr io_ratio is input/output; our ratio is output/input.
    soxr_set_io_ratio(static_cast<soxr_t>(soxr_),
                      initial_io_ratio > 0.0 ? 1.0 / initial_io_ratio : 1.0, 0);
}

Resampler::~Resampler() {
    if (soxr_ != nullptr) {
        soxr_delete(static_cast<soxr_t>(soxr_));
    }
}

std::size_t Resampler::process(const float* in, std::size_t in_frames, float* out,
                               std::size_t out_cap, double ratio) {
    soxr_t s = static_cast<soxr_t>(soxr_);
    soxr_set_io_ratio(s, ratio > 0.0 ? 1.0 / ratio : 1.0, 0);

    std::size_t idone = 0;
    std::size_t odone = 0;
    soxr_process(s, in, in_frames, &idone, out, out_cap, &odone);
    return odone;
}

} // namespace audiopipe
