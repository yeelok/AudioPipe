#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "audiopipe/drift_control.h"
#include "audiopipe/resampler.h"
#include "audiopipe/ring_buffer.h"

using namespace audiopipe;

namespace {

constexpr int         kChannels     = 2;
constexpr int         kPeriod       = 96; // consumer frames per tick
constexpr std::size_t kPeriodFloats = std::size_t(kPeriod) * kChannels; // 192
constexpr std::size_t kTarget       = 8 * kPeriodFloats;  // 1536
constexpr std::size_t kCapacity     = 64 * kPeriodFloats; // 12288

struct SimResult {
    std::size_t underruns  = 0;
    std::size_t overflows  = 0;
    double      min_fill   = 0.0;
    double      max_fill   = 0.0;
    double      final_fill = 0.0;
};

// Deterministic model of the receiver pipeline: a producer delivers samples at
// (1 + ppm/1e6) the consumer rate; the variable-rate resampler + PI drift
// controller sit between producer and a lock-free ring drained at the device
// period. Mirrors tools/drift_sim (kept short for CI).
SimResult run_sim(double ppm, bool correct, std::size_t ticks) {
    constexpr std::size_t kWarmup = 200;

    SpscRingBuffer<float> ring(kCapacity);
    Resampler             rs(1.0, kChannels);
    DriftController       dc(kTarget, kCapacity);

    std::vector<float> prefill(kTarget, 0.0f);
    ring.push(prefill.data(), prefill.size());

    const double       rp_over_rc = 1.0 + ppm / 1.0e6;
    const std::size_t  out_cap    = std::size_t(kPeriod) * 4 + 64; // frames
    std::vector<float> in_buf, out_buf(out_cap * kChannels), consume(kPeriodFloats);
    double             producer_accum = 0.0;
    double             phase          = 0.0;

    SimResult r;
    r.min_fill = r.max_fill = double(ring.size());

    for (std::size_t t = 0; t < ticks; ++t) {
        producer_accum += double(kPeriod) * rp_over_rc;
        const std::size_t in_frames = std::size_t(producer_accum);
        producer_accum -= double(in_frames);

        in_buf.resize(in_frames * kChannels);
        for (std::size_t n = 0; n < in_frames; ++n) {
            const float s = 0.5f * std::sin(phase);
            phase += 0.05;
            if (phase > 6.283185307) phase -= 6.283185307;
            for (int c = 0; c < kChannels; ++c) {
                in_buf[n * kChannels + c] = s;
            }
        }

        const double ratio = correct ? dc.update(ring.size()) : 1.0;
        const std::size_t out_frames =
            rs.process(in_buf.data(), in_frames, out_buf.data(), out_cap, ratio);

        const std::size_t want   = out_frames * kChannels;
        const std::size_t pushed = ring.push(out_buf.data(), want);
        const std::size_t popped = ring.pop(consume.data(), kPeriodFloats);

        if (t >= kWarmup) {
            if (pushed < want) ++r.overflows;
            if (popped < kPeriodFloats) ++r.underruns;
            const double fill = double(ring.size());
            if (fill < r.min_fill) r.min_fill = fill;
            if (fill > r.max_fill) r.max_fill = fill;
        }
    }
    r.final_fill = double(ring.size());
    return r;
}

} // namespace

TEST_CASE("drift correction keeps the buffer centered at +200 ppm", "[drift]") {
    const SimResult r = run_sim(+200.0, /*correct=*/true, 60000); // ~2 min
    CHECK(r.underruns == 0);
    CHECK(r.overflows == 0);
    CHECK(std::fabs(r.final_fill - double(kTarget)) < double(2 * kPeriodFloats));
    CHECK(r.min_fill > double(2 * kPeriodFloats));      // never near empty
    CHECK(r.max_fill < double(kTarget + 8 * kPeriodFloats)); // never near full
}

TEST_CASE("drift correction keeps the buffer centered at -200 ppm", "[drift]") {
    const SimResult r = run_sim(-200.0, /*correct=*/true, 60000);
    CHECK(r.underruns == 0);
    CHECK(r.overflows == 0);
    CHECK(std::fabs(r.final_fill - double(kTarget)) < double(2 * kPeriodFloats));
    CHECK(r.min_fill > double(2 * kPeriodFloats));
    CHECK(r.max_fill < double(kTarget + 8 * kPeriodFloats));
}

TEST_CASE("without correction the buffer drifts away from target", "[drift]") {
    const SimResult r = run_sim(+200.0, /*correct=*/false, 60000);
    // The control case must drift markedly — proof the loop is real and that
    // correction is doing the work in the cases above.
    CHECK(std::fabs(r.final_fill - double(kTarget)) > double(4 * kPeriodFloats));
}
