// Loopback harness (M1): runs audio through the real wire path —
// frames -> §7 packets -> UDP over 127.0.0.1 -> jitter buffer -> frames —
// with optional packet loss / reordering injected, then reports how faithfully
// the output matches the input. Generates a sine test signal by default, or
// reads a WAV with --in. Writes <prefix>-input.wav and <prefix>-output.wav.
//
// POSIX sockets; built on macOS/Linux only (see tools/CMakeLists.txt). The
// production sender/receiver get their own platform socket code in M3/M4.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "audiopipe/format.h"
#include "audiopipe/jitter_buffer.h"
#include "audiopipe/packet.h"
#include "wav.h"

using namespace audiopipe;
using audiopipe::tools::WavData;

namespace {

struct Options {
    int           frame_samples = kDefaultFrameSamples;
    std::uint16_t channels      = kChannels;
    std::uint32_t rate          = kSampleRate;
    double        seconds       = 2.0;
    double        drop          = 0.0; // P(packet dropped)
    double        reorder       = 0.0; // P(packet delayed past the next one)
    std::uint16_t port          = 4010;
    std::uint16_t jitter        = 3;
    std::uint32_t seed          = 1;
    std::string   in_path;
    std::string   out_prefix = "loopback";
};

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                return std::string();
            }
            return argv[++i];
        };
        if (a == "--frames") o.frame_samples = std::stoi(next("--frames"));
        else if (a == "--channels") o.channels = std::uint16_t(std::stoi(next("--channels")));
        else if (a == "--rate") o.rate = std::uint32_t(std::stoul(next("--rate")));
        else if (a == "--seconds") o.seconds = std::stod(next("--seconds"));
        else if (a == "--drop") o.drop = std::stod(next("--drop"));
        else if (a == "--reorder") o.reorder = std::stod(next("--reorder"));
        else if (a == "--port") o.port = std::uint16_t(std::stoi(next("--port")));
        else if (a == "--jitter") o.jitter = std::uint16_t(std::stoi(next("--jitter")));
        else if (a == "--seed") o.seed = std::uint32_t(std::stoul(next("--seed")));
        else if (a == "--in") o.in_path = next("--in");
        else if (a == "--out-prefix") o.out_prefix = next("--out-prefix");
        else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return false;
        }
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
            const double t    = double(n) / rate;
            w.samples[n * channels + c] = 0.5f * float(std::sin(kTwoPi * freq * t));
        }
    }
    return w;
}

int make_recv_socket(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int rcvbuf = 4 * 1024 * 1024;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    timeval tv{};
    tv.tv_usec = 150000; // 150 ms idle timeout
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        return 2;
    }

    // --- Build the input signal -------------------------------------------
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

    const std::size_t frame_floats = std::size_t(opt.frame_samples) * opt.channels;
    if (frame_floats == 0) {
        std::fprintf(stderr, "invalid frame/channel configuration\n");
        return 1;
    }
    // Pad to a whole number of frames.
    if (input.samples.size() % frame_floats != 0) {
        input.samples.resize(((input.samples.size() / frame_floats) + 1) * frame_floats, 0.0f);
    }
    const std::size_t num_frames = input.samples.size() / frame_floats;

    tools::write_wav_f32(opt.out_prefix + "-input.wav", input);

    // --- Sockets ----------------------------------------------------------
    const int recv_fd = make_recv_socket(opt.port);
    if (recv_fd < 0) {
        std::fprintf(stderr, "failed to bind UDP port %u: %s\n", opt.port, std::strerror(errno));
        return 1;
    }
    const int send_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(opt.port);
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    // --- Receiver thread: arrivals -> jitter buffer -> playout ------------
    // Output is produced in lock-step with arrivals: once the priming depth is
    // reached, one frame is popped per inserted packet, mirroring real playout
    // and keeping buffer fill ~= depth so the finite reorder window never
    // overflows. Any remaining frames are drained after the sender finishes.
    JitterBuffer jb(opt.jitter, std::uint16_t(opt.frame_samples), opt.channels);
    std::atomic<bool>        sender_done{false};
    std::atomic<std::size_t> received{0};
    std::vector<float>       out_samples(num_frames * frame_floats, 0.0f);

    std::thread receiver([&] {
        std::vector<std::uint8_t> buf(packet_size(frame_floats));
        std::vector<float>        payload(frame_floats);
        std::size_t               inserts = 0, popped = 0;
        for (;;) {
            const ssize_t n = ::recvfrom(recv_fd, buf.data(), buf.size(), 0, nullptr, nullptr);
            if (n > 0) {
                PacketHeader h{};
                if (decode_header(buf.data(), std::size_t(n), h) &&
                    decode_payload(buf.data(), std::size_t(n), h, payload.data(),
                                   payload.size()) == frame_floats) {
                    jb.insert(h.sequence, payload.data(), frame_floats);
                    received.fetch_add(1, std::memory_order_relaxed);
                    if (++inserts > opt.jitter && popped < num_frames) {
                        jb.pop_frame(&out_samples[popped * frame_floats]);
                        ++popped;
                    }
                }
            } else if (sender_done.load(std::memory_order_acquire)) {
                break; // timeout with nothing more coming
            }
        }
        while (popped < num_frames) { // drain the priming cushion + any gaps
            jb.pop_frame(&out_samples[popped * frame_floats]);
            ++popped;
        }
    });

    // --- Sender: packetize every frame, inject loss/reorder ---------------
    std::mt19937                           rng(opt.seed);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::vector<std::uint8_t>              datagram(packet_size(frame_floats));
    std::optional<std::vector<std::uint8_t>> held;
    std::size_t                            sent = 0, dropped = 0;

    auto send = [&](const std::vector<std::uint8_t>& d) {
        ::sendto(send_fd, d.data(), d.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        ++sent;
    };

    for (std::size_t f = 0; f < num_frames; ++f) {
        PacketHeader h{};
        h.magic       = kMagic;
        h.sequence    = std::uint16_t(f);
        h.timestamp   = std::uint64_t(f) * opt.frame_samples;
        h.frame_count = std::uint16_t(opt.frame_samples);
        h.channels    = std::uint8_t(opt.channels);
        h.format_id   = std::uint8_t(FormatId::Float32);
        encode(h, &input.samples[f * frame_floats], frame_floats, datagram.data(), datagram.size());

        if (uni(rng) < opt.drop) {
            ++dropped;
            continue;
        }
        if (opt.reorder > 0.0 && !held && uni(rng) < opt.reorder) {
            held = datagram; // hold this one back, send after the next
            continue;
        }
        send(datagram);
        if (held) {
            send(*held);
            held.reset();
        }
    }
    if (held) {
        send(*held);
        held.reset();
    }

    sender_done.store(true, std::memory_order_release);
    receiver.join();
    ::close(send_fd);
    ::close(recv_fd);

    // --- Assemble the output ----------------------------------------------
    WavData output;
    output.sample_rate = opt.rate;
    output.channels    = opt.channels;
    output.samples     = std::move(out_samples);
    tools::write_wav_f32(opt.out_prefix + "-output.wav", output);

    // --- Compare & report -------------------------------------------------
    std::size_t mismatched = 0;
    double      max_abs = 0.0, sse = 0.0;
    for (std::size_t i = 0; i < output.samples.size(); ++i) {
        const double d = std::fabs(double(output.samples[i]) - double(input.samples[i]));
        if (d != 0.0) ++mismatched;
        if (d > max_abs) max_abs = d;
        sse += d * d;
    }
    const double rms = std::sqrt(sse / double(output.samples.size()));

    std::printf("loopback harness\n");
    std::printf("  frames=%zu  frame_samples=%d  channels=%u  rate=%u  packet=%zu bytes\n",
                num_frames, opt.frame_samples, opt.channels, opt.rate, packet_size(frame_floats));
    std::printf("  inject: drop=%.3f reorder=%.3f  jitter_depth=%u  seed=%u\n", opt.drop,
                opt.reorder, opt.jitter, opt.seed);
    std::printf("  packets: sent=%zu dropped_at_source=%zu received=%zu\n", sent, dropped,
                received.load());
    std::printf("  jitter:  concealed=%llu late_dropped=%llu overflow=%llu\n",
                (unsigned long long)jb.lost_count(),
                (unsigned long long)jb.dropped_late_count(),
                (unsigned long long)jb.dropped_overflow_count());
    std::printf("  output:  mismatched_samples=%zu max_abs_err=%.3g rms_err=%.3g\n", mismatched,
                max_abs, rms);
    const bool clean_path = (opt.drop == 0.0 && opt.reorder == 0.0);
    if (clean_path) {
        std::printf("  bit-transparent (no faults): %s\n", mismatched == 0 ? "YES" : "NO");
        return mismatched == 0 ? 0 : 1;
    }
    std::printf("  (faults injected; concealment expected to differ from source)\n");
    return 0;
}
