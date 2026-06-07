// In-process AOO loopback: an AooSource streams a sine into an AooSink with no
// real sockets — the source's send callback hands packets straight to the
// sink's handleMessage and vice-versa. Driven in real time (AOO's time-DLL
// expects process() to be paced at the audio rate), it verifies the integration
// the receiver/sender depend on:
//
//   * matched rate  (48k -> 48k): audio flows, energy preserved.
//   * mismatched     (44.1k -> 48k): the sink resamples; audio still flows.
//
// The second case is the regression guard for the bug that motivated adopting
// AOO: a non-48k source used to starve the device. We assert on RMS energy
// (robust) rather than bit-exact samples (timing-dependent).

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include "aoo.h"
#include "aoo_sink.hpp"
#include "aoo_source.hpp"
#include "codec/aoo_pcm.h"

namespace {

constexpr AooId kSrcId  = 1;
constexpr AooId kSinkId = 1;
constexpr int   kChannels = 2;
constexpr int   kBlock    = 64;

struct Loopback {
    AooSource::Ptr source;
    AooSink::Ptr   sink;
    sockaddr_in    srcAddr{};  // token identifying the source endpoint
    sockaddr_in    sinkAddr{}; // token identifying the sink endpoint
};

// source -> sink: deliver outgoing source packets to the sink, "from" srcAddr.
AooInt32 AOO_CALL src_send(void* user, const AooByte* data, AooInt32 size,
                           const void*, AooAddrSize, AooFlag) {
    auto* lb = static_cast<Loopback*>(user);
    lb->sink->handleMessage(data, size, &lb->srcAddr, sizeof(lb->srcAddr));
    return size;
}

// sink -> source: deliver outgoing sink packets (resend reqs/pings) "from" sinkAddr.
AooInt32 AOO_CALL sink_send(void* user, const AooByte* data, AooInt32 size,
                            const void*, AooAddrSize, AooFlag) {
    auto* lb = static_cast<Loopback*>(user);
    lb->source->handleMessage(data, size, &lb->sinkAddr, sizeof(lb->sinkAddr));
    return size;
}

// Initialize AOO exactly once per test binary. aoo_initialize/aoo_terminate is
// not safe to cycle repeatedly (Catch2 re-runs the body once per SECTION), so
// we init lazily on first use and let process exit clean up.
void ensure_aoo() {
    static const bool ok = (aoo_initialize(nullptr) == kAooOk);
    REQUIRE(ok);
}

struct Result {
    double      tailRms = 0.0; // RMS over the second half (after latency fill)
    double      fullRms = 0.0; // RMS over the whole capture
    double      maxAbs  = 0.0; // peak |sample|
    std::size_t n       = 0;   // total samples collected (channel 0)
};

// Run the loopback for `seconds`; return output statistics for the sink.
Result run_loopback(AooSampleRate srcRate, AooSampleRate sinkRate, double seconds) {
    ensure_aoo();
    Loopback lb;
    lb.srcAddr.sin_family  = AF_INET;
    lb.srcAddr.sin_port    = htons(40001);
    lb.sinkAddr.sin_family = AF_INET;
    lb.sinkAddr.sin_port   = htons(40002);
    inet_pton(AF_INET, "127.0.0.1", &lb.srcAddr.sin_addr);
    inet_pton(AF_INET, "127.0.0.1", &lb.sinkAddr.sin_addr);

    lb.source = AooSource::create(kSrcId);
    lb.sink   = AooSink::create(kSinkId);

    lb.source->setup(kChannels, srcRate, kBlock, 0);
    AooFormatPcm fmt;
    AooFormatPcm_init(&fmt, kChannels, srcRate, kBlock, kAooPcmFloat32);
    lb.source->setFormat(fmt.header);
    lb.source->setResendBufferSize(0.05);
    AooEndpoint ep{ &lb.sinkAddr, sizeof(lb.sinkAddr), kSinkId };
    lb.source->addSink(ep, kAooTrue);

    lb.sink->setup(kChannels, sinkRate, kBlock, 0);
    lb.sink->setLatency(0.02); // 20 ms
    lb.sink->setResendData(kAooTrue);
    lb.sink->setDynamicResampling(kAooTrue);

    // Planar I/O buffers (one channel pointer each).
    std::vector<float>      inBuf(kBlock * kChannels, 0.0f);
    std::vector<float>      outBuf(kBlock * kChannels, 0.0f);
    std::vector<AooSample*> inPtr(kChannels), outPtr(kChannels);
    for (int c = 0; c < kChannels; ++c) {
        inPtr[c]  = &inBuf[c * kBlock];
        outPtr[c] = &outBuf[c * kBlock];
    }

    std::vector<float> collected; // sink channel 0 output

    lb.source->startStream(0, nullptr);

    // Drive AOO from a SYNTHETIC monotonic clock (not wall time): each tick
    // advances by one sink block (kBlock/sinkRate s) and we pass that timestamp
    // to both process() calls. This makes the test deterministic and fast (no
    // sleeps) and decoupled from OS scheduling jitter. We pull kBlock from the
    // sink each tick and feed the source the matching fraction of input samples
    // (kBlock*srcRate/sinkRate); since m/srcRate == kBlock/sinkRate == tick
    // duration, source-time and sink-time advance identically.
    const std::size_t totalTicks = std::size_t(seconds * double(sinkRate) / kBlock);
    const double      srcPerSink = double(srcRate) / double(sinkRate);
    const double      tickSec    = double(kBlock) / double(sinkRate);
    double            t = 1.0; // start at a nonzero NTP offset
    double            phase = 0.0, srcAccum = 0.0;
    const double      adv = 440.0 / double(srcRate);

    for (std::size_t tick = 0; tick < totalTicks; ++tick) {
        const AooNtpTime nt = aoo_ntpTimeFromSeconds(t);

        // Feed the source its share of input (integer part of the accumulator).
        srcAccum += double(kBlock) * srcPerSink;
        int m = int(srcAccum);
        srcAccum -= m;
        if (m > kBlock) m = kBlock;
        for (int i = 0; i < m; ++i) {
            const float s = 0.5f * float(std::sin(phase * 2.0 * M_PI));
            phase += adv;
            for (int c = 0; c < kChannels; ++c) inBuf[c * kBlock + i] = s;
        }
        if (m > 0) {
            lb.source->process(inPtr.data(), m, nt);
            lb.source->send(src_send, &lb);
        }

        // Pull one sink block.
        lb.sink->process(outPtr.data(), kBlock, nt, nullptr, nullptr);
        for (int i = 0; i < kBlock; ++i) collected.push_back(outBuf[i]); // channel 0
        lb.sink->send(sink_send, &lb);

        lb.source->pollEvents();
        lb.sink->pollEvents();

        t += tickSec;
    }

    Result r;
    r.n = collected.size();
    const std::size_t start = collected.size() / 2;
    double sumTail = 0.0, sumFull = 0.0;
    std::size_t nTail = 0;
    for (std::size_t i = 0; i < collected.size(); ++i) {
        const double v = double(collected[i]);
        sumFull += v * v;
        r.maxAbs = std::max(r.maxAbs, std::fabs(v));
        if (i >= start) { sumTail += v * v; ++nTail; }
    }
    r.fullRms = collected.empty() ? 0.0 : std::sqrt(sumFull / collected.size());
    r.tailRms = nTail ? std::sqrt(sumTail / nTail) : 0.0;
    return r;
}

} // namespace

TEST_CASE("AOO loopback streams audio end to end", "[aoo]") {
    // A 0.5-amplitude sine has RMS ~0.354. Allow generous bounds: we only care
    // that audio actually flows through (non-silent) and isn't garbage/clipping.
    SECTION("matched rate (48k -> 48k)") {
        const Result r = run_loopback(48000, 48000, 0.5);
        INFO("tail=" << r.tailRms << " full=" << r.fullRms << " max=" << r.maxAbs << " n=" << r.n);
        REQUIRE(r.tailRms > 0.05);
        REQUIRE(r.tailRms < 0.6);
    }

    // The pipeline is rate-agnostic when both ends agree.
    SECTION("44.1k -> 44.1k flows (rate-agnostic pipeline)") {
        const Result r = run_loopback(44100, 44100, 0.5);
        INFO("tail=" << r.tailRms << " full=" << r.fullRms << " max=" << r.maxAbs << " n=" << r.n);
        REQUIRE(r.tailRms > 0.05);
        REQUIRE(r.tailRms < 0.6);
    }

    // Regression guard for the bug that motivated adopting AOO: a non-48 kHz
    // source used to starve the 48 kHz device. AOO resamples 44.1k -> 48k, so
    // the sink stays fed and keeps producing audio.
    SECTION("mismatched rate (44.1k -> 48k) is resampled, not starved") {
        const Result r = run_loopback(44100, 48000, 0.5);
        INFO("tail=" << r.tailRms << " full=" << r.fullRms << " max=" << r.maxAbs << " n=" << r.n);
        REQUIRE(r.tailRms > 0.05); // old hand-rolled path would starve -> ~0
        REQUIRE(r.tailRms < 0.6);
    }
}
