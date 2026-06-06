// AudioPipe macOS receiver (M3): receives §7 UDP packets, runs them through the
// jitter buffer + drift correction, and plays the result into the AudioPipe
// virtual device (the BlackHole fork). Apps select "AudioPipe" as an input and
// hear the stream. Separate process per spec §6.3 (embedding into the plug-in
// is the optional M9 path).
//
//   net thread:  recvfrom -> JitterBuffer.insert -> (primed) pop frame ->
//                Resampler(drift ratio) -> SpscRingBuffer
//   CoreAudio:   HAL output unit render callback pops the ring into the device
//
// macOS only (CoreAudio / AudioToolbox).

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audiopipe/config.h"
#include "audiopipe/drift_control.h"
#include "audiopipe/format.h"
#include "audiopipe/jitter_buffer.h"
#include "audiopipe/log.h"
#include "audiopipe/packet.h"
#include "audiopipe/resampler.h"
#include "audiopipe/ring_buffer.h"

using namespace audiopipe;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

struct RenderCtx {
    SpscRingBuffer<float>*   ring;
    int                      channels;
    std::atomic<std::uint64_t> underruns{0};
};

// CoreAudio realtime callback: pop interleaved float frames from the ring;
// fill any shortfall with silence. Lock-free and allocation-free.
OSStatus render_cb(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32,
                   UInt32 frames, AudioBufferList* io) {
    auto* ctx = static_cast<RenderCtx*>(ref);
    auto* out = static_cast<float*>(io->mBuffers[0].mData);
    const std::size_t need = std::size_t(frames) * ctx->channels;
    const std::size_t got  = ctx->ring->pop(out, need);
    if (got < need) {
        std::memset(out + got, 0, (need - got) * sizeof(float));
        ctx->underruns.fetch_add(1, std::memory_order_relaxed);
    }
    return noErr;
}

// Find a CoreAudio device by name that has output streams (we play into it).
AudioDeviceID find_output_device(const std::string& name) {
    AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDevices,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr) {
        return kAudioObjectUnknown;
    }
    std::vector<AudioDeviceID> ids(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size,
                                   ids.data()) != noErr) {
        return kAudioObjectUnknown;
    }

    for (AudioDeviceID id : ids) {
        CFStringRef cf = nullptr;
        UInt32      ns = sizeof(cf);
        AudioObjectPropertyAddress na = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain};
        if (AudioObjectGetPropertyData(id, &na, 0, nullptr, &ns, &cf) != noErr || cf == nullptr) {
            continue;
        }
        char buf[256] = {0};
        CFStringGetCString(cf, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(cf);
        if (name != buf) {
            continue;
        }
        AudioObjectPropertyAddress sa = {kAudioDevicePropertyStreams,
                                         kAudioObjectPropertyScopeOutput,
                                         kAudioObjectPropertyElementMain};
        UInt32 ss = 0;
        if (AudioObjectGetPropertyDataSize(id, &sa, 0, nullptr, &ss) == noErr && ss > 0) {
            return id;
        }
    }
    return kAudioObjectUnknown;
}

UInt32 device_buffer_frames(AudioDeviceID dev) {
    UInt32 frames = 512, sz = sizeof(frames);
    AudioObjectPropertyAddress a = {kAudioDevicePropertyBufferFrameSize,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &frames);
    return frames;
}

// Request a small device IO buffer to keep latency low (best effort; the
// device clamps to its supported range). Latency is tuned for real in M5.
void request_buffer_frames(AudioDeviceID dev, UInt32 want) {
    AudioObjectPropertyAddress a = {kAudioDevicePropertyBufferFrameSize,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    AudioObjectSetPropertyData(dev, &a, 0, nullptr, sizeof(want), &want);
}

// Network + feeder thread: arrivals -> jitter buffer -> drift-corrected
// resampler -> ring. The jitter buffer's frame size is learned from the first
// packet (the sender fixes it per session).
void net_thread(const ReceiverConfig cfg, SpscRingBuffer<float>* ring, std::size_t target_fill,
                std::size_t capacity, Logger* log) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        log->info("net: socket() failed");
        return;
    }
    int rcvbuf = 4 * 1024 * 1024;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    timeval tv{};
    tv.tv_usec = 200000; // 200 ms, so we can poll g_stop
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(cfg.listen_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        log->info("net: bind() failed");
        ::close(fd);
        return;
    }

    std::vector<std::uint8_t> buf(2048);
    std::unique_ptr<JitterBuffer> jb;
    std::unique_ptr<Resampler>    rs;
    DriftController               dc(target_fill, capacity);
    std::vector<float>            payload, frame, out;
    std::size_t                   frame_floats = 0, inserts = 0;
    std::uint64_t                 pkts = 0, last_log_lost = 0;

    while (!g_stop.load(std::memory_order_acquire)) {
        const ssize_t n = ::recvfrom(fd, buf.data(), buf.size(), 0, nullptr, nullptr);
        if (n <= 0) {
            continue; // timeout (poll g_stop) or error
        }
        PacketHeader h{};
        if (!decode_header(buf.data(), std::size_t(n), h)) {
            continue;
        }

        if (!jb) { // lazy init from the first packet's geometry
            frame_floats = std::size_t(h.frame_count) * h.channels;
            jb  = std::make_unique<JitterBuffer>(cfg.jitter_frames, h.frame_count, h.channels);
            rs  = std::make_unique<Resampler>(1.0, h.channels);
            payload.assign(frame_floats, 0.0f);
            frame.assign(frame_floats, 0.0f);
            out.assign(frame_floats * 4 + 64, 0.0f);
            log->info("net: stream started (" + std::to_string(h.frame_count) + " frames, " +
                      std::to_string(h.channels) + "ch)");
        }

        if (decode_payload(buf.data(), std::size_t(n), h, payload.data(), payload.size()) !=
            frame_floats) {
            continue;
        }
        jb->insert(h.sequence, payload.data(), frame_floats);
        ++pkts;

        // Lock-step playout: one frame out per arrival once primed.
        if (++inserts > cfg.jitter_frames) {
            jb->pop_frame(frame.data());
            const double ratio = dc.update(ring->size());
            const std::size_t in_frames  = frame_floats / h.channels;
            const std::size_t out_frames = rs->process(frame.data(), in_frames, out.data(),
                                                        out.size() / h.channels, ratio);
            ring->push(out.data(), out_frames * h.channels);
        }

        if ((pkts % 2400) == 0) { // ~ every 5 s at 2 ms frames
            const std::uint64_t lost = jb->lost_count();
            log->info("net: pkts=" + std::to_string(pkts) + " concealed=" + std::to_string(lost) +
                      " (+" + std::to_string(lost - last_log_lost) + ") drift_ratio=" +
                      std::to_string(dc.ratio()) + " ring=" + std::to_string(ring->size()));
            last_log_lost = lost;
        }
    }
    ::close(fd);
    log->info("net: stopped, total packets=" + std::to_string(pkts));
}

} // namespace

int main(int argc, char** argv) {
    std::string config_path = "config/receiver.json";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    ReceiverConfig cfg;
    try {
        cfg = load_receiver_config(config_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "config error: %s\n", e.what());
        return 1;
    }

    Logger log(cfg.log_file);
    log.info("receiver: starting (port=" + std::to_string(cfg.listen_port) + " device='" +
             cfg.device_name + "')");
    std::fprintf(stderr, "AudioPipe receiver: port %u -> device '%s'\n", cfg.listen_port,
                 cfg.device_name.c_str());

    const AudioDeviceID dev = find_output_device(cfg.device_name);
    if (dev == kAudioObjectUnknown) {
        std::fprintf(stderr, "device '%s' not found (is the driver installed?)\n",
                     cfg.device_name.c_str());
        log.info("receiver: device not found");
        return 1;
    }

    request_buffer_frames(dev, 128); // low-latency request; device may clamp
    const UInt32      dev_buf      = device_buffer_frames(dev);
    const std::size_t dev_floats   = std::size_t(dev_buf) * cfg.channels;
    const std::size_t target_fill  = 3 * dev_floats;  // ring cushion (drift target)
    const std::size_t ring_capacity = 16 * dev_floats;
    SpscRingBuffer<float> ring(ring_capacity);

    // --- Set up the HAL output unit on the AudioPipe device ---------------
    AudioComponentDescription desc = {kAudioUnitType_Output, kAudioUnitSubType_HALOutput,
                                      kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    AudioUnit      unit = nullptr;
    if (comp == nullptr || AudioComponentInstanceNew(comp, &unit) != noErr) {
        std::fprintf(stderr, "failed to create HAL output unit\n");
        return 1;
    }

    UInt32 enable = 1, disable = 0;
    AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0,
                         &enable, sizeof(enable));
    AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1,
                         &disable, sizeof(disable));
    AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
                         &dev, sizeof(dev));

    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate       = cfg.sample_rate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked; // interleaved
    fmt.mFramesPerPacket  = 1;
    fmt.mChannelsPerFrame = cfg.channels;
    fmt.mBitsPerChannel   = 32;
    fmt.mBytesPerFrame    = cfg.channels * 4;
    fmt.mBytesPerPacket   = cfg.channels * 4;
    AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt,
                         sizeof(fmt));

    RenderCtx ctx;
    ctx.ring     = &ring;
    ctx.channels = cfg.channels;
    AURenderCallbackStruct cb = {render_cb, &ctx};
    AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb,
                         sizeof(cb));

    if (AudioUnitInitialize(unit) != noErr || AudioOutputUnitStart(unit) != noErr) {
        std::fprintf(stderr, "failed to start audio unit\n");
        return 1;
    }
    log.info("receiver: audio unit started (dev_buf=" + std::to_string(dev_buf) +
             " target=" + std::to_string(target_fill) + " cap=" + std::to_string(ring_capacity) +
             ")");

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::thread net(net_thread, cfg, &ring, target_fill, ring_capacity, &log);
    net.join();

    AudioOutputUnitStop(unit);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    log.info("receiver: stopped (render underruns=" +
             std::to_string(ctx.underruns.load()) + ")");
    std::fprintf(stderr, "receiver: stopped\n");
    return 0;
}
