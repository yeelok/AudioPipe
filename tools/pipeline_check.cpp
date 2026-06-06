// pipeline_check: run real audio through the FULL receiver DSP pipeline offline
// and listen to / measure the result, with no network and no audio hardware.
//
// It mirrors receiver/main.cpp's real-time path exactly:
//   producer frames -> JitterBuffer (priming, reorder, loss concealment)
//                    -> pop_frame (one per arrival, once primed)
//                    -> DriftController (PI on ring fill) -> Resampler (libsoxr)
//                    -> SpscRingBuffer
//   consumer        -> pop a device period each tick at the receiver clock rate
//
// This is the piece the other tools don't cover end-to-end on real audio:
// loopback_harness stops at the jitter buffer (no resampler/ring); drift_sim
// runs the resampler/ring but on a synthetic sine and only checks buffer
// stability. pipeline_check runs a real WAV through all of it and writes an
// output WAV you can listen to, while injecting the conditions that matter
// (clock drift, packet loss/reorder). Portable C++17, builds on Windows + macOS.
//
//   pipeline_check --in song.wav --out out.wav
//   pipeline_check --in song.wav --ppm 500            # sender clock +500 ppm
//   pipeline_check --in song.wav --drop 0.02 --reorder 0.05
//   pipeline_check --ppm 500 --no-correct             # generated sine; should GLITCH

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "audiopipe/drift_control.h"
#include "audiopipe/format.h"
#include "audiopipe/jitter_buffer.h"
#include "audiopipe/resampler.h"
#include "audiopipe/ring_buffer.h"
#include "wav.h"

using namespace audiopipe;
using audiopipe::tools::WavData;

namespace {

struct Options {
    int           frame_samples = kDefaultFrameSamples; // jitter-buffer frame size
    std::uint16_t channels      = kChannels;
    std::uint32_t rate          = kSampleRate;
    double        seconds       = 4.0;   // generated-sine length when no --in
    double        ppm           = 0.0;   // sender clock vs receiver clock offset
    double        drop          = 0.0;   // P(packet lost)
    double        reorder       = 0.0;   // P(packet delayed past the next)
    std::uint16_t jitter_frames = 3;     // jitter buffer depth (spec §8.1)
    bool          correct       = true;  // drift correction on/off
    std::uint32_t seed          = 1;
    std::string   in_path;
    std::string   out_path = "pipeline-output.wav";
};

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", name); return {}; }
            return argv[++i];
        };
        if (a == "--frames") o.frame_samples = std::stoi(next("--frames"));
        else if (a == "--channels") o.channels = std::uint16_t(std::stoi(next("--channels")));
        else if (a == "--rate") o.rate = std::uint32_t(std::stoul(next("--rate")));
        else if (a == "--seconds") o.seconds = std::stod(next("--seconds"));
        else if (a == "--ppm") o.ppm = std::stod(next("--ppm"));
        else if (a == "--drop") o.drop = std::stod(next("--drop"));
        else if (a == "--reorder") o.reorder = std::stod(next("--reorder"));
        else if (a == "--jitter-frames") o.jitter_frames = std::uint16_t(std::stoi(next("--jitter-frames")));
        else if (a == "--no-correct") o.correct = false;
        else if (a == "--seed") o.seed = std::uint32_t(std::stoul(next("--seed")));
        else if (a == "--in") o.in_path = next("--in");
        else if (a == "--out") o.out_path = next("--out");
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return false; }
    }
    return true;
}

WavData generate_sine(std::uint32_t rate, std::uint16_t channels, double seconds) {
    WavData w;
    w.sample_rate = rate;
    w.channels    = channels;
    const std::size_t frames = std::size_t(seconds * rate);
    w.samples.resize(frames * channels);
    constexpr double kTwoPi = 6.283185307179586;
    for (std::size_t n = 0; n < frames; ++n) {
        for (std::uint16_t c = 0; c < channels; ++c) {
            const double freq = 220.0 * (c + 2); // 440, 660, ... per channel
            w.samples[n * channels + c] = 0.4f * float(std::sin(kTwoPi * freq * double(n) / rate));
        }
    }
    return w;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) return 2;

    // --- Input -----------------------------------------------------------
    WavData input;
    if (!opt.in_path.empty()) {
        std::string err;
        if (!tools::read_wav(opt.in_path, input, err)) {
            std::fprintf(stderr, "read_wav: %s\n", err.c_str());
            return 1;
        }
        opt.rate     = input.sample_rate;
        opt.channels = input.channels;
    } else {
        input = generate_sine(opt.rate, opt.channels, opt.seconds);
    }

    const int         ch           = opt.channels;
    const std::size_t frame_floats = std::size_t(opt.frame_samples) * ch;
    if (frame_floats == 0) { std::fprintf(stderr, "invalid frame/channel config\n"); return 1; }
    if (input.samples.size() % frame_floats != 0) {
        input.samples.resize(((input.samples.size() / frame_floats) + 1) * frame_floats, 0.0f);
    }
    const std::size_t num_frames = input.samples.size() / frame_floats;

    // --- Pipeline state (mirrors receiver/main.cpp) ----------------------
    const std::size_t dev_floats = frame_floats;        // consumer device period
    const std::size_t target     = 3 * dev_floats;      // ring drift target
    const std::size_t capacity   = 16 * dev_floats;     // ring capacity

    JitterBuffer    jb(opt.jitter_frames, std::uint16_t(opt.frame_samples), opt.channels);
    Resampler       rs(1.0, ch);
    DriftController dc(target, capacity);
    SpscRingBuffer<float> ring(capacity);

    std::vector<float> prime(target, 0.0f);             // primed cushion (no startup underrun)
    ring.push(prime.data(), prime.size());

    const std::size_t  out_cap = std::size_t(opt.frame_samples) * 4 + 64; // resampler frames
    std::vector<float> frame(frame_floats), rsout(out_cap * ch);
    std::vector<float> consume((std::size_t(opt.frame_samples) + 8) * ch); // ~one period +slack
    std::vector<float> out_samples;
    out_samples.reserve(input.samples.size() + dev_floats * 8);

    // Fault injection (same scheme as loopback_harness).
    std::mt19937                           rng(opt.seed);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::optional<std::size_t>             held; // frame index delayed by reorder

    // Consumer drains at the receiver clock; sender runs at (1+ppm/1e6) of it.
    const double consume_per_tick = double(opt.frame_samples) / (1.0 + opt.ppm / 1.0e6);
    double       consume_accum    = 0.0;

    const std::size_t warmup = 64 + opt.jitter_frames; // ignore startup for health counters
    std::size_t inserts = 0, produced = 0, dropped_src = 0, inserted = 0;
    std::size_t underruns = 0, overflows = 0;
    std::size_t min_fill = ring.size(), max_fill = ring.size();
    double      last_ratio = 1.0;

    auto playout = [&](std::size_t frame_index) {
        jb.insert(std::uint16_t(frame_index), &input.samples[frame_index * frame_floats], frame_floats);
        ++inserted;
        if (++inserts > opt.jitter_frames) {
            jb.pop_frame(frame.data());
            last_ratio = opt.correct ? dc.update(ring.size()) : 1.0;
            const std::size_t outf =
                rs.process(frame.data(), opt.frame_samples, rsout.data(), out_cap, last_ratio);
            const std::size_t want = outf * ch;
            const std::size_t got  = ring.push(rsout.data(), want);
            if (inserts > warmup && got < want) ++overflows;
        }
    };

    for (std::size_t f = 0; f < num_frames; ++f) {
        ++produced;
        // Sender side: drop / reorder this frame, else feed it to the jitter buffer.
        if (uni(rng) < opt.drop) {
            ++dropped_src; // lost in flight; jitter buffer will conceal the gap
        } else if (opt.reorder > 0.0 && !held && uni(rng) < opt.reorder) {
            held = f;      // hold back; insert after the next one
        } else {
            playout(f);
            if (held) { playout(*held); held.reset(); }
        }

        // Consumer side: drain ~one device period (scaled by the clock offset)
        // this tick. `n` is samples-per-channel, so it is a single pop.
        consume_accum += consume_per_tick;
        const std::size_t n    = std::size_t(consume_accum);
        consume_accum         -= double(n);
        const std::size_t want = n * ch;
        const std::size_t got  = ring.pop(consume.data(), want);
        if (got < want) {
            std::fill(consume.begin() + got, consume.begin() + want, 0.0f); // device plays silence
            if (inserts > warmup) ++underruns;
        }
        out_samples.insert(out_samples.end(), consume.begin(), consume.begin() + want);

        if (inserts > warmup) {
            const std::size_t fill = ring.size();
            min_fill = std::min(min_fill, fill);
            max_fill = std::max(max_fill, fill);
        }
    }
    if (held) { playout(*held); held.reset(); }

    // --- Output WAV ------------------------------------------------------
    WavData output;
    output.sample_rate = opt.rate;
    output.channels    = opt.channels;
    output.samples     = out_samples;
    if (!tools::write_wav_f32(opt.out_path, output)) {
        std::fprintf(stderr, "failed to write %s\n", opt.out_path.c_str());
        return 1;
    }

    // --- Output signal health -------------------------------------------
    double      peak = 0.0, sse = 0.0;
    std::size_t nan = 0, clipped = 0;
    for (float v : out_samples) {
        if (std::isnan(v) || std::isinf(v)) { ++nan; continue; }
        const double a = std::fabs(double(v));
        if (a > peak) peak = a;
        if (a > 1.0) ++clipped;
        sse += double(v) * double(v);
    }
    const double rms = out_samples.empty() ? 0.0 : std::sqrt(sse / double(out_samples.size()));

    std::printf("pipeline_check (jitter -> drift resampler -> ring -> consumer)\n");
    std::printf("  in=%s  frames=%zu frame_samples=%d ch=%d rate=%u\n",
                opt.in_path.empty() ? "<sine>" : opt.in_path.c_str(),
                num_frames, opt.frame_samples, ch, opt.rate);
    std::printf("  inject: ppm=%+.1f drop=%.3f reorder=%.3f jitter_frames=%u correct=%s seed=%u\n",
                opt.ppm, opt.drop, opt.reorder, opt.jitter_frames, opt.correct ? "yes" : "no",
                opt.seed);
    std::printf("  packets: produced=%zu dropped_in_flight=%zu inserted=%zu\n",
                produced, dropped_src, inserted);
    std::printf("  jitter:  concealed=%llu late_dropped=%llu overflow=%llu\n",
                (unsigned long long)jb.lost_count(),
                (unsigned long long)jb.dropped_late_count(),
                (unsigned long long)jb.dropped_overflow_count());
    std::printf("  ring:    target=%zu min_fill=%zu max_fill=%zu cap=%zu final_ratio=%.6f\n",
                target, min_fill, max_fill, capacity, last_ratio);
    std::printf("  device:  underruns=%zu ring_overflows=%zu\n", underruns, overflows);
    std::printf("  output:  frames=%zu rms=%.4f peak=%.4f NaN/Inf=%zu clipped=%zu  -> %s\n",
                out_samples.size() / std::size_t(ch), rms, peak, nan, clipped, opt.out_path.c_str());

    // A run with no injected loss/reorder should be STABLE (the resampler is
    // near-transparent and drift correction holds the ring centered). Loss/
    // reorder runs are informational (concealment legitimately alters output).
    const bool fault_free = (opt.drop == 0.0 && opt.reorder == 0.0);
    const bool healthy     = (underruns == 0 && overflows == 0 && nan == 0);
    if (fault_free) {
        std::printf("  RESULT: %s\n", healthy ? "STABLE" : "GLITCHED");
        return healthy ? 0 : 1;
    }
    std::printf("  RESULT: faults injected (concealed=%llu) -- inspect %s by ear\n",
                (unsigned long long)jb.lost_count(), opt.out_path.c_str());
    return 0;
}
