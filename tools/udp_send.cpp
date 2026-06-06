// Real-time §7 packet sender (M3 testing): reads a WAV (or generates a sine)
// and streams it as AudioPipe UDP packets, paced to real time. Stands in for
// the Windows sender so the macOS receiver + AudioPipe device can be exercised
// end-to-end on one machine:
//
//   build/receiver/audiopipe_receiver --config config/receiver.json
//   build/tools/udp_send --in music.wav --host 127.0.0.1 --port 4010 --loop
//
// POSIX sockets; macOS/Linux only.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "audiopipe/format.h"
#include "audiopipe/packet.h"
#include "wav.h"

using namespace audiopipe;
using audiopipe::tools::WavData;

namespace {

struct Options {
    std::string   host    = "127.0.0.1";
    std::uint16_t port    = 4010;
    int           frames  = kDefaultFrameSamples;
    std::uint16_t channels = kChannels;
    std::uint32_t rate    = kSampleRate;
    double        seconds = 5.0;
    bool          loop    = false;
    bool          pace    = true;
    std::string   in_path;
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&] { return std::string(argv[++i]); };
        if (a == "--host") o.host = val();
        else if (a == "--port") o.port = std::uint16_t(std::stoi(val()));
        else if (a == "--frames") o.frames = std::stoi(val());
        else if (a == "--channels") o.channels = std::uint16_t(std::stoi(val()));
        else if (a == "--rate") o.rate = std::uint32_t(std::stoul(val()));
        else if (a == "--seconds") o.seconds = std::stod(val());
        else if (a == "--in") o.in_path = val();
        else if (a == "--loop") o.loop = true;
        else if (a == "--no-pace") o.pace = false;
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

} // namespace

int main(int argc, char** argv) {
    const Options opt = parse(argc, argv);

    WavData audio;
    if (!opt.in_path.empty()) {
        std::string err;
        if (!tools::read_wav(opt.in_path, audio, err)) {
            std::fprintf(stderr, "read_wav: %s\n", err.c_str());
            return 1;
        }
    } else {
        audio = generate_sine(opt.rate, opt.channels, opt.seconds);
    }

    const std::size_t frame_floats = std::size_t(opt.frames) * audio.channels;
    if (frame_floats == 0) {
        std::fprintf(stderr, "invalid frame/channel config\n");
        return 1;
    }
    if (audio.samples.size() % frame_floats != 0) {
        audio.samples.resize(((audio.samples.size() / frame_floats) + 1) * frame_floats, 0.0f);
    }
    const std::size_t num_frames = audio.samples.size() / frame_floats;

    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return 1;
    }
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(opt.port);
    if (::inet_pton(AF_INET, opt.host.c_str(), &dst.sin_addr) != 1) {
        std::fprintf(stderr, "bad host: %s\n", opt.host.c_str());
        return 1;
    }

    std::fprintf(stderr, "udp_send -> %s:%u  %zu frames x %d samples x %uch @ %u Hz%s\n",
                 opt.host.c_str(), opt.port, num_frames, opt.frames, audio.channels,
                 audio.sample_rate, opt.loop ? " (loop)" : "");

    std::vector<std::uint8_t> datagram(packet_size(frame_floats));
    const auto frame_dur = std::chrono::nanoseconds(
        std::int64_t(1e9 * double(opt.frames) / double(audio.sample_rate)));

    std::uint16_t seq   = 0;
    std::uint64_t tstamp = 0;
    auto          next   = std::chrono::steady_clock::now();
    do {
        for (std::size_t f = 0; f < num_frames; ++f) {
            PacketHeader h{};
            h.magic       = kMagic;
            h.sequence    = seq++;
            h.timestamp   = tstamp;
            h.frame_count = std::uint16_t(opt.frames);
            h.channels    = std::uint8_t(audio.channels);
            h.format_id   = std::uint8_t(FormatId::Float32);
            tstamp += opt.frames;

            encode(h, &audio.samples[f * frame_floats], frame_floats, datagram.data(),
                   datagram.size());
            ::sendto(fd, datagram.data(), datagram.size(), 0, reinterpret_cast<sockaddr*>(&dst),
                     sizeof(dst));

            if (opt.pace) {
                next += frame_dur;
                std::this_thread::sleep_until(next);
            }
        }
    } while (opt.loop);

    ::close(fd);
    return 0;
}
