// pipeline_check: run real audio through the FULL AOO pipeline offline — no
// network, no audio hardware — and write a listenable output WAV plus stats.
//
// It wires an AooSource straight into an AooSink in-process (the source's send
// callback hands packets to the sink's handleMessage and vice-versa), driven by
// a synthetic monotonic clock so it's deterministic and runs faster than real
// time. This is the same engine receiver/main.cpp and the Windows sender use
// (jitter buffer + time-DLL drift correction + dynamic resampling + resend), so
// you can audition what a given network/clock condition actually sounds like:
//
//   pipeline_check --in song.wav --out out.wav
//   pipeline_check --in song.wav --drop 0.02 --reorder 0.05   # loss + reorder (resend recovers)
//   pipeline_check --in song.wav --rate 44100                 # 44.1k source -> 48k sink (resample)
//   pipeline_check --ppm 500                                  # simulate a +500 ppm source clock
//   pipeline_check --ppm 500 --no-correct                     # disable drift correction (both ends)
//
// Complements tests/test_aoo_loopback.cpp (which asserts energy on a sine, no
// faults) by running a real WAV with injected faults and producing audio you can
// listen to. Loss, reorder and cross-rate resampling are reproduced faithfully;
// clock-drift (`--ppm`) exercises the path but its audible effect is understated
// here — the synthetic single-process clock can't model two independent hardware
// clocks the way a real two-machine link does (see docs/plan.md). POSIX sockets
// are used only as opaque endpoint tokens; builds where the other tools build
// (macOS/Linux, see tools/CMakeLists.txt).

#include <arpa/inet.h>
#include <netinet/in.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "aoo.h"
#include "aoo_sink.hpp"
#include "aoo_source.hpp"
#include "codec/aoo_pcm.h"

#include "wav.h"

using audiopipe::tools::WavData;

namespace {

constexpr AooId kSrcId  = 1;
constexpr AooId kSinkId = 1;
constexpr double kPi    = 3.14159265358979323846;

struct Options {
    std::string in_path;
    std::string out_path = "pipeline_out.wav";
    std::uint32_t rate      = 48000;  // source rate (try 44100 for cross-rate)
    std::uint32_t sink_rate = 48000;  // device rate
    std::uint16_t channels  = 2;
    int           block     = 96;
    double        seconds   = 5.0;    // sine length when no --in
    double        drop      = 0.0;    // P(packet dropped)
    double        reorder   = 0.0;    // P(packet delayed past the next)
    double        ppm       = 0.0;    // source clock offset
    double        latency   = 0.05;   // sink jitter-buffer latency (s)
    bool          correct   = true;   // dynamic resampling on the sink
    std::uint32_t seed      = 1;
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&] { return std::string(argv[++i]); };
        if (a == "--in") o.in_path = val();
        else if (a == "--out") o.out_path = val();
        else if (a == "--rate") o.rate = std::uint32_t(std::stoul(val()));
        else if (a == "--sink-rate") o.sink_rate = std::uint32_t(std::stoul(val()));
        else if (a == "--channels") o.channels = std::uint16_t(std::stoi(val()));
        else if (a == "--block") o.block = std::stoi(val());
        else if (a == "--seconds") o.seconds = std::stod(val());
        else if (a == "--drop") o.drop = std::stod(val());
        else if (a == "--reorder") o.reorder = std::stod(val());
        else if (a == "--ppm") o.ppm = std::stod(val());
        else if (a == "--latency") o.latency = std::stod(val()) / 1000.0; // arg in ms
        else if (a == "--no-correct") o.correct = false;
        else if (a == "--seed") o.seed = std::uint32_t(std::stoul(val()));
    }
    return o;
}

// In-process plumbing shared by the send callbacks + the loop.
struct Pipe {
    AooSource::Ptr source;
    AooSink::Ptr   sink;
    sockaddr_in    srcAddr{};   // opaque identity token for the source endpoint
    sockaddr_in    sinkAddr{};  // opaque identity token for the sink endpoint
    // fault injection (source -> sink path)
    double                    drop = 0.0, reorder = 0.0;
    std::mt19937              rng;
    std::uniform_real_distribution<double> uni{0.0, 1.0};
    std::vector<AooByte>      held;        // a packet delayed past the next (reorder)
    bool                      hasHeld = false;
    std::uint64_t             sent = 0, dropped = 0, reordered = 0;
    // sink-side AOO event tallies
    std::uint64_t xrun = 0, overrun = 0, underrun = 0, resend = 0, blockdrop = 0, started = 0;
};

void deliver_to_sink(Pipe& p, const AooByte* d, AooInt32 n) {
    p.sink->handleMessage(d, n, &p.srcAddr, sizeof(p.srcAddr));
}

// source -> sink, with drop/reorder injection.
AooInt32 AOO_CALL src_send(void* user, const AooByte* data, AooInt32 size,
                           const void*, AooAddrSize, AooFlag) {
    auto& p = *static_cast<Pipe*>(user);
    if (p.drop > 0.0 && p.uni(p.rng) < p.drop) { // drop this packet
        ++p.dropped;
        return size; // tell AOO it went out (lost on the "wire")
    }
    if (p.reorder > 0.0 && !p.hasHeld && p.uni(p.rng) < p.reorder) {
        p.held.assign(data, data + size); // hold it back, send after the next
        p.hasHeld = true;
        ++p.reordered;
        return size;
    }
    deliver_to_sink(p, data, size);
    ++p.sent;
    if (p.hasHeld) {
        deliver_to_sink(p, p.held.data(), AooInt32(p.held.size()));
        p.hasHeld = false;
    }
    return size;
}

// sink -> source (resend requests / pings); delivered intact.
AooInt32 AOO_CALL sink_send(void* user, const AooByte* data, AooInt32 size,
                            const void*, AooAddrSize, AooFlag) {
    auto& p = *static_cast<Pipe*>(user);
    p.source->handleMessage(data, size, &p.sinkAddr, sizeof(p.sinkAddr));
    return size;
}

void AOO_CALL on_sink_event(void* user, const AooEvent* e, AooThreadLevel) {
    auto& p = *static_cast<Pipe*>(user);
    switch (e->type) {
    case kAooEventStreamStart:    ++p.started; break;
    case kAooEventBlockXRun:      ++p.xrun; break;
    case kAooEventBufferOverrun:  ++p.overrun; break;
    case kAooEventBufferUnderrun: ++p.underrun; break;
    case kAooEventBlockResend:    ++p.resend; break;
    case kAooEventFrameResend:    ++p.resend; break;
    case kAooEventBlockDrop:      ++p.blockdrop; break;
    default: break;
    }
}

WavData generate_sine(std::uint32_t rate, std::uint16_t ch, double seconds) {
    WavData w;
    w.sample_rate = rate;
    w.channels    = ch;
    const std::size_t frames = std::size_t(seconds * rate);
    w.samples.resize(frames * ch);
    for (std::size_t n = 0; n < frames; ++n)
        for (std::uint16_t c = 0; c < ch; ++c)
            w.samples[n * ch + c] = 0.5f * float(std::sin(2.0 * kPi * 440.0 * double(n) / rate));
    return w;
}

double rms(const std::vector<float>& v, std::size_t stride, std::size_t ch0) {
    if (v.empty()) return 0.0;
    double s = 0.0; std::size_t n = 0;
    for (std::size_t i = ch0; i < v.size(); i += stride, ++n) s += double(v[i]) * double(v[i]);
    return n ? std::sqrt(s / n) : 0.0;
}

} // namespace

int main(int argc, char** argv) {
    const Options opt = parse(argc, argv);

    WavData in;
    if (!opt.in_path.empty()) {
        std::string err;
        if (!audiopipe::tools::read_wav(opt.in_path, in, err)) {
            std::fprintf(stderr, "read_wav: %s\n", err.c_str());
            return 1;
        }
    } else {
        in = generate_sine(opt.rate, opt.channels, opt.seconds);
    }
    const std::uint16_t ch       = in.channels;
    const std::uint32_t srcRate  = in.sample_rate;
    const std::uint32_t sinkRate = opt.sink_rate;
    const int           block    = opt.block;
    const std::size_t   inFrames = in.samples.size() / ch;
    if (ch == 0 || block <= 0 || inFrames == 0) {
        std::fprintf(stderr, "bad input (channels/block/frames)\n");
        return 1;
    }

    if (aoo_initialize(nullptr) != kAooOk) {
        std::fprintf(stderr, "aoo_initialize failed\n");
        return 1;
    }

    Pipe p;
    p.drop = opt.drop; p.reorder = opt.reorder; p.rng.seed(opt.seed);
    p.srcAddr.sin_family  = AF_INET; p.srcAddr.sin_port  = htons(40001);
    p.sinkAddr.sin_family = AF_INET; p.sinkAddr.sin_port = htons(40002);
    inet_pton(AF_INET, "127.0.0.1", &p.srcAddr.sin_addr);
    inet_pton(AF_INET, "127.0.0.1", &p.sinkAddr.sin_addr);

    constexpr AooInt32 kMaxBlock = 8192; // process() chunk ceiling (drift can exceed `block`)
    p.source = AooSource::create(kSrcId);
    p.source->setup(ch, AooSampleRate(srcRate), kMaxBlock, 0);
    AooFormatPcm fmt;
    AooFormatPcm_init(&fmt, ch, AooInt32(srcRate), block, kAooPcmFloat32);
    p.source->setFormat(fmt.header);
    p.source->setResendBufferSize(0.2);
    p.source->setBufferSize(0.1);
    // --no-correct disables drift compensation on BOTH ends (otherwise the
    // source resamples the drift away before it ever reaches the sink).
    p.source->setDynamicResampling(opt.correct ? kAooTrue : kAooFalse);
    AooEndpoint ep{&p.sinkAddr, sizeof(p.sinkAddr), kSinkId};
    p.source->addSink(ep, kAooTrue);

    p.sink = AooSink::create(kSinkId);
    p.sink->setup(ch, AooSampleRate(sinkRate), block, 0);
    p.sink->setLatency(opt.latency);
    p.sink->setBufferSize(std::max(opt.latency * 4.0, 0.08));
    p.sink->setResendData(kAooTrue);
    p.sink->setDynamicResampling(opt.correct ? kAooTrue : kAooFalse);
    p.sink->setEventHandler(on_sink_event, &p, kAooEventModePoll);

    // Output spans the same wall-clock duration as the input, at the sink rate.
    const double      durSec     = double(inFrames) / double(srcRate);
    const std::size_t totalTicks = std::size_t(durSec * double(sinkRate) / block);
    const double      drift      = 1.0 + opt.ppm / 1.0e6;
    const double      tickSec    = double(block) / double(sinkRate);

    std::vector<float>      inBlk(std::size_t(kMaxBlock) * ch, 0.0f);
    std::vector<float>      outBlk(std::size_t(block) * ch, 0.0f);
    std::vector<AooSample*> inPtr(ch), outPtr(ch);
    std::vector<float>      out; out.reserve(totalTicks * std::size_t(block) * ch);

    p.source->startStream(0, nullptr);

    double tSink = 1.0, tSrc = 1.0, srcAccum = 0.0;
    std::size_t cursor = 0; // input read position (frames), loops the file

    for (std::size_t t = 0; t < totalTicks; ++t) {
        // Feed the source srcRate*tickSec*drift samples this tick (fractional
        // carry), labelled with the (drifting) source clock tSrc.
        srcAccum += double(srcRate) * tickSec * drift;
        int m = int(srcAccum); srcAccum -= m;
        if (m > kMaxBlock) m = kMaxBlock;
        for (int i = 0; i < m; ++i) {
            const std::size_t f = (cursor + i) % inFrames;
            for (std::uint16_t c = 0; c < ch; ++c)
                inBlk[std::size_t(c) * m + i] = in.samples[f * ch + c];
        }
        cursor = (cursor + m) % inFrames;
        for (std::uint16_t c = 0; c < ch; ++c) inPtr[c] = &inBlk[std::size_t(c) * m];
        if (m > 0) {
            p.source->process(inPtr.data(), m, aoo_ntpTimeFromSeconds(tSrc));
            p.source->send(src_send, &p);
        }
        tSrc += tickSec * drift;

        // Pull one sink block at the steady sink clock.
        for (std::uint16_t c = 0; c < ch; ++c) outPtr[c] = &outBlk[std::size_t(c) * block];
        p.sink->process(outPtr.data(), block, aoo_ntpTimeFromSeconds(tSink), nullptr, nullptr);
        for (int i = 0; i < block; ++i)
            for (std::uint16_t c = 0; c < ch; ++c) out.push_back(outBlk[std::size_t(c) * block + i]);
        p.sink->send(sink_send, &p);
        p.source->pollEvents();
        p.sink->pollEvents();
        tSink += tickSec;
    }

    WavData outw;
    outw.sample_rate = sinkRate;
    outw.channels    = ch;
    outw.samples     = std::move(out);
    const bool wrote = audiopipe::tools::write_wav_f32(opt.out_path, outw);

    std::printf("pipeline_check\n");
    std::printf("  in:   %s  %zu frames x %uch @ %u Hz\n",
                opt.in_path.empty() ? "(sine)" : opt.in_path.c_str(), inFrames, ch, srcRate);
    std::printf("  sink: %u Hz  latency=%.0f ms  dynamic_resampling=%s\n",
                sinkRate, opt.latency * 1000.0, opt.correct ? "on" : "OFF");
    std::printf("  inject: drop=%.3f reorder=%.3f drift=%+.0f ppm\n", opt.drop, opt.reorder, opt.ppm);
    std::printf("  packets: delivered=%llu dropped=%llu reordered=%llu\n",
                (unsigned long long)p.sent, (unsigned long long)p.dropped,
                (unsigned long long)p.reordered);
    std::printf("  sink events: stream_start=%llu xrun=%llu overrun=%llu underrun=%llu resend=%llu blockdrop=%llu\n",
                (unsigned long long)p.started, (unsigned long long)p.xrun,
                (unsigned long long)p.overrun, (unsigned long long)p.underrun,
                (unsigned long long)p.resend, (unsigned long long)p.blockdrop);
    std::printf("  audio: in_rms=%.4f out_rms=%.4f  out=%s (%zu frames)\n",
                rms(in.samples, ch, 0), rms(outw.samples, ch, 0),
                wrote ? opt.out_path.c_str() : "(WRITE FAILED)",
                outw.samples.size() / ch);

    p.source.reset();
    p.sink.reset();
    aoo_terminate();
    return wrote ? 0 : 1;
}
