// AOO real-time sender (macOS test tool): reads a WAV (or generates a sine)
// and streams it to an AooSink via an AooSource, paced to real time. Stands in
// for the Windows sender so the macOS receiver + AudioPipe device can be
// exercised end-to-end on one machine:
//
//   build/receiver/audiopipe_receiver --config config/receiver.json
//   build/tools/aoo_send --in music.wav --host 127.0.0.1 --port 4010 --loop
//
// The source streams at the WAV's own sample rate; the sink resamples to the
// device rate. Streaming a 44.1 kHz file therefore plays cleanly (the old
// hand-rolled path starved the device because it could not resample).
//
// POSIX sockets; macOS/Linux only.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "aoo.h"
#include "aoo_source.hpp"
#include "codec/aoo_pcm.h"

#include "wav.h"

using audiopipe::tools::WavData;

namespace {

constexpr AooId kSourceId = 1;
constexpr AooId kSinkId   = 1;

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

struct Options {
    std::string   host    = "127.0.0.1";
    std::uint16_t port    = 4010;
    int           block   = 96;          // samples/channel per AOO block (2 ms @ 48 k)
    std::uint16_t channels = 2;
    std::uint32_t rate    = 48000;
    double        seconds = 5.0;         // sine duration when no WAV
    bool          loop    = false;
    std::string   in_path;
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&] { return std::string(argv[++i]); };
        if (a == "--host") o.host = val();
        else if (a == "--port") o.port = std::uint16_t(std::stoi(val()));
        else if (a == "--block") o.block = std::stoi(val());
        else if (a == "--channels") o.channels = std::uint16_t(std::stoi(val()));
        else if (a == "--rate") o.rate = std::uint32_t(std::stoul(val()));
        else if (a == "--seconds") o.seconds = std::stod(val());
        else if (a == "--in") o.in_path = val();
        else if (a == "--loop") o.loop = true;
    }
    return o;
}

WavData generate_sine(std::uint32_t rate, std::uint16_t ch, double seconds) {
    WavData w;
    w.sample_rate = rate;
    w.channels    = ch;
    const std::size_t frames = std::size_t(seconds * rate);
    w.samples.resize(frames * ch);
    constexpr double kTwoPi = 6.283185307179586;
    for (std::size_t n = 0; n < frames; ++n) {
        for (std::uint16_t c = 0; c < ch; ++c) {
            const double f = 220.0 * (c + 2);
            w.samples[n * ch + c] = 0.5f * float(std::sin(kTwoPi * f * (double(n) / rate)));
        }
    }
    return w;
}

AooInt32 AOO_CALL udp_send(void* user, const AooByte* data, AooInt32 size,
                           const void* address, AooAddrSize addrlen, AooFlag) {
    int fd = *static_cast<int*>(user);
    auto n = ::sendto(fd, data, std::size_t(size), 0,
                      static_cast<const sockaddr*>(address), addrlen);
    return AooInt32(n);
}

} // namespace

int main(int argc, char** argv) {
    const Options opt = parse(argc, argv);

    WavData audio;
    if (!opt.in_path.empty()) {
        std::string err;
        if (!audiopipe::tools::read_wav(opt.in_path, audio, err)) {
            std::fprintf(stderr, "read_wav: %s\n", err.c_str());
            return 1;
        }
    } else {
        audio = generate_sine(opt.rate, opt.channels, opt.seconds);
    }
    const std::uint16_t ch   = audio.channels;
    const std::uint32_t rate = audio.sample_rate;
    const std::size_t   block = std::size_t(opt.block);
    if (ch == 0 || block == 0) {
        std::fprintf(stderr, "invalid channels/block\n");
        return 1;
    }
    const std::size_t num_frames = audio.samples.size() / ch;

    if (aoo_initialize(nullptr) != kAooOk) {
        std::fprintf(stderr, "aoo_initialize failed\n");
        return 1;
    }

    // UDP socket + destination endpoint.
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { std::perror("socket"); return 1; }
    timeval tv{}; tv.tv_usec = 2000; // 2 ms: flush outgoing audio promptly
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(opt.port);
    if (::inet_pton(AF_INET, opt.host.c_str(), &dst.sin_addr) != 1) {
        std::fprintf(stderr, "bad host: %s\n", opt.host.c_str());
        return 1;
    }

    // AOO source.
    AooSource::Ptr source = AooSource::create(kSourceId);
    source->setup(ch, AooSampleRate(rate), AooInt32(block), 0);
    AooFormatPcm fmt;
    AooFormatPcm_init(&fmt, ch, AooInt32(rate), AooInt32(block), kAooPcmFloat32); // PCM rate is AooInt32
    source->setFormat(fmt.header);
    source->setResendBufferSize(0.05);     // keep 50 ms of history for resends
    source->setBufferSize(0.1);            // 100 ms send queue (AOO default 25 ms): burst headroom so
                                           // process() can't overflow -> add_xrun -> sink "block xrun"
    source->setDynamicResampling(kAooTrue); // measure our real send rate for the sink's DLL
    AooEndpoint ep{ &dst, sizeof(dst), kSinkId };
    source->addSink(ep, kAooTrue);

    std::fprintf(stderr, "aoo_send -> %s:%u  %zu frames x %dch @ %u Hz block=%zu%s\n",
                 opt.host.c_str(), opt.port, num_frames, ch, rate, block,
                 opt.loop ? " (loop)" : "");

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // Network thread: flush outgoing packets + handle resend requests.
    std::atomic<bool> net_done{false};
    std::thread net([&] {
        std::vector<AooByte> buf(AOO_MAX_PACKET_SIZE);
        while (!net_done.load(std::memory_order_acquire)) {
            source->send(udp_send, &fd);
            sockaddr_storage src{};
            socklen_t        srclen = sizeof(src);
            const ssize_t n = ::recvfrom(fd, buf.data(), buf.size(), 0,
                                         reinterpret_cast<sockaddr*>(&src), &srclen);
            if (n > 0) {
                source->handleMessage(buf.data(), AooInt32(n), &src, srclen);
            }
            source->pollEvents();
        }
    });

    source->startStream(0, nullptr);

    // Pacing thread (here, main): feed one block per block-duration.
    std::vector<float>      planar_data(block * ch, 0.0f);
    std::vector<AooSample*> ptrs(ch);
    for (std::uint16_t c = 0; c < ch; ++c) ptrs[c] = &planar_data[c * block];

    const auto block_dur = std::chrono::nanoseconds(
        std::int64_t(1e9 * double(block) / double(rate)));
    auto next = std::chrono::steady_clock::now();

    do {
        for (std::size_t f0 = 0; f0 < num_frames && !g_stop.load(); f0 += block) {
            const std::size_t n = std::min(block, num_frames - f0);
            for (std::size_t i = 0; i < block; ++i) {
                for (std::uint16_t c = 0; c < ch; ++c) {
                    planar_data[c * block + i] =
                        (i < n) ? audio.samples[(f0 + i) * ch + c] : 0.0f;
                }
            }
            source->process(ptrs.data(), AooInt32(block), aoo_getCurrentNtpTime());
            // NB: a hybrid sleep+busy-spin pacing was tried here to remove OS
            // timer-coalescing jitter; it did NOT reduce the receiver's low-
            // latency buffer swing (the jitter is receiver/scheduling-side on a
            // single contended box, not sender pacing — see docs/plan.md), so we
            // keep the lightweight sleep_until.
            next += block_dur;
            std::this_thread::sleep_until(next);
        }
    } while (opt.loop && !g_stop.load());

    source->stopStream(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // let /stop flush
    net_done.store(true, std::memory_order_release);
    net.join();
    ::close(fd);
    source.reset();
    aoo_terminate();
    return 0;
}
