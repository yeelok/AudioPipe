// Drift simulation (M2): a deterministic, no-wall-clock model of the receiver
// pipeline under a sender/consumer clock offset. A producer delivers samples at
// rate Rp = 48000 * (1 + ppm/1e6); a consumer pulls a fixed device period each
// tick at rate Rc = 48000. Between them sit the variable-rate resampler and the
// PI drift controller (driven by ring-buffer fill). Reports whether the buffer
// stays centered with no under/overrun — the core of spec §8.4 / the "multi-
// hour, no cumulative drift" acceptance criterion, validated in accelerated form.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "audiopipe/drift_control.h"
#include "audiopipe/resampler.h"
#include "audiopipe/ring_buffer.h"

using namespace audiopipe;

namespace {

struct Options {
    double ppm          = 200.0; // sender faster (+) or slower (-) than consumer
    double minutes      = 5.0;
    int    channels     = 2;
    int    period       = 96; // consumer frames/tick (samples per channel)
    bool   correct      = true;
    bool   trace        = false;
    double kp = -1, ki = -1, max_dev = -1; // <0 == use library defaults
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&] { return argv[++i]; };
        if (a == "--ppm") o.ppm = std::stod(val());
        else if (a == "--minutes") o.minutes = std::stod(val());
        else if (a == "--channels") o.channels = std::stoi(val());
        else if (a == "--period") o.period = std::stoi(val());
        else if (a == "--no-correct") o.correct = false;
        else if (a == "--trace") o.trace = true;
        else if (a == "--kp") o.kp = std::stod(val());
        else if (a == "--ki") o.ki = std::stod(val());
        else if (a == "--max-dev") o.max_dev = std::stod(val());
    }
    return o;
}

} // namespace

int main(int argc, char** argv) {
    const Options opt = parse(argc, argv);

    const std::size_t period_floats = std::size_t(opt.period) * opt.channels;
    const std::size_t target        = 8 * period_floats;  // priming cushion
    const std::size_t capacity      = 64 * period_floats; // ring usable capacity
    const std::size_t ticks         = std::size_t(opt.minutes * 60.0 * 48000.0 / opt.period);
    const std::size_t warmup        = 200; // ignore resampler startup transient

    SpscRingBuffer<float> ring(capacity);
    Resampler             rs(1.0, opt.channels);
    DriftController       dc(target, capacity);
    if (opt.kp >= 0.0) {
        dc.set_gains(opt.kp, opt.ki, opt.max_dev);
    }

    std::vector<float> prefill(target, 0.0f);
    ring.push(prefill.data(), prefill.size());

    const double            rp_over_rc = 1.0 + opt.ppm / 1.0e6;
    const std::size_t       out_cap    = std::size_t(opt.period) * 4 + 64; // frames
    std::vector<float>      in_buf, out_buf(out_cap * opt.channels), consume(period_floats);
    double                  producer_accum = 0.0;
    double                  phase          = 0.0;

    std::size_t underruns = 0, overflows = 0;
    double      min_fill = double(ring.size()), max_fill = double(ring.size());
    double      last_ratio = 1.0;

    for (std::size_t t = 0; t < ticks; ++t) {
        producer_accum += double(opt.period) * rp_over_rc;
        const std::size_t in_frames = std::size_t(producer_accum);
        producer_accum -= double(in_frames);

        in_buf.resize(in_frames * opt.channels);
        for (std::size_t n = 0; n < in_frames; ++n) {
            const float s = 0.5f * std::sin(phase);
            phase += 0.05;
            if (phase > 6.283185307) phase -= 6.283185307;
            for (int c = 0; c < opt.channels; ++c) {
                in_buf[n * opt.channels + c] = s;
            }
        }

        last_ratio = opt.correct ? dc.update(ring.size()) : 1.0;
        const std::size_t out_frames =
            rs.process(in_buf.data(), in_frames, out_buf.data(), out_cap, last_ratio);

        const std::size_t want   = out_frames * opt.channels;
        const std::size_t pushed = ring.push(out_buf.data(), want);
        const std::size_t popped = ring.pop(consume.data(), period_floats);

        if (t >= warmup) {
            if (pushed < want) ++overflows;
            if (popped < period_floats) ++underruns;
            const double fill = double(ring.size());
            min_fill = std::min(min_fill, fill);
            max_fill = std::max(max_fill, fill);
        }
        if (opt.trace && t % (ticks / 20 + 1) == 0) {
            std::printf("  t=%-8zu fill=%-7zu ratio=%.6f\n", t, ring.size(), last_ratio);
        }
    }

    const double final_fill = double(ring.size());
    std::printf("drift_sim: ppm=%+.1f minutes=%.1f correct=%s ticks=%zu period=%d ch=%d\n",
                opt.ppm, opt.minutes, opt.correct ? "yes" : "no", ticks, opt.period,
                opt.channels);
    std::printf("  target_fill=%zu capacity=%zu\n", target, capacity);
    std::printf("  fill: min=%.0f max=%.0f final=%.0f (drift from target=%+.0f)\n", min_fill,
                max_fill, final_fill, final_fill - double(target));
    std::printf("  under/overruns: under=%zu over=%zu  final_ratio=%.6f\n", underruns, overflows,
                last_ratio);
    const bool ok = (underruns == 0 && overflows == 0);
    std::printf("  result: %s\n", ok ? "STABLE" : "GLITCHED");
    return ok ? 0 : 1;
}
