// AudioPipe macOS receiver: receives an AOO stream over UDP and plays it into
// the AudioPipe virtual device (the BlackHole fork). Apps select "AudioPipe" as
// an input and hear the stream.
//
//   net thread:  recvfrom -> AooSink::handleMessage; AooSink::send (resend
//                requests / pings back to the source); AooSink::pollEvents
//   CoreAudio:   the HAL render callback calls AooSink::process(), which runs
//                the jitter buffer + time-DLL drift correction + dynamic
//                resampling and writes the next block of audio.
//
// AOO (deps/aoo) owns the wire protocol, jitter buffering, clock-drift
// correction and sample-rate conversion. Because the sink resamples the source
// stream to the device rate, a non-48 kHz source no longer starves the device
// (the old hand-rolled pipeline only had a +/-0.1% drift trim).
//
// macOS only (CoreAudio / AudioToolbox).

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "aoo.h"
#include "aoo_sink.hpp"

#include "audiopipe/config.h"
#include "audiopipe/log.h"

using namespace audiopipe;

namespace {

constexpr AooId   kSinkId   = 1;
constexpr AooInt32 kMaxBlock = 4096; // upper bound for a single render block

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

struct RenderCtx {
    AooSink*               sink     = nullptr;
    int                    channels = 2;
    std::vector<std::vector<float>> planar;  // per-channel scratch
    std::vector<AooSample*>         ptrs;    // pointers into `planar`
};

// CoreAudio realtime callback: pull the next block from the AOO sink (planar),
// then interleave into the device buffer. AooSink::process fills silence
// internally when the jitter buffer is empty (reported as underrun events).
OSStatus render_cb(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32,
                   UInt32 frames, AudioBufferList* io) {
    auto* ctx = static_cast<RenderCtx*>(ref);
    auto* out = static_cast<float*>(io->mBuffers[0].mData);
    const int ch = ctx->channels;

    if (frames > UInt32(kMaxBlock)) { // never expected; stay safe
        std::memset(out, 0, std::size_t(frames) * ch * sizeof(float));
        return noErr;
    }
    for (int c = 0; c < ch; ++c) {
        ctx->ptrs[c] = ctx->planar[c].data();
    }
    ctx->sink->process(ctx->ptrs.data(), AooInt32(frames), aoo_getCurrentNtpTime(),
                       nullptr, nullptr);
    for (UInt32 n = 0; n < frames; ++n) {
        for (int c = 0; c < ch; ++c) {
            out[n * ch + c] = ctx->planar[c][n];
        }
    }
    return noErr;
}

// UDP send function handed to AooSink::send (sends control packets back to the
// source — resend requests, pings). AOO supplies the destination address.
AooInt32 AOO_CALL udp_send(void* user, const AooByte* data, AooInt32 size,
                           const void* address, AooAddrSize addrlen, AooFlag) {
    int fd = *static_cast<int*>(user);
    auto n = ::sendto(fd, data, std::size_t(size), 0,
                      static_cast<const sockaddr*>(address), addrlen);
    return AooInt32(n);
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

void request_buffer_frames(AudioDeviceID dev, UInt32 want) {
    AudioObjectPropertyAddress a = {kAudioDevicePropertyBufferFrameSize,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    AudioObjectSetPropertyData(dev, &a, 0, nullptr, sizeof(want), &want);
}

Logger* g_log = nullptr;

// Source endpoint + buffer-fill swing tracking. Touched only on the net thread
// (the event handler runs there via pollEvents, as does the fill sampler), so
// no synchronization is needed.
sockaddr_storage g_src_ss{};
AooAddrSize      g_src_len  = 0;
AooId            g_src_id   = 0;
bool             g_have_src = false;
double           g_fill_min = 1e9, g_fill_max = -1e9;

// Forward AOO's internal log (warnings/errors, e.g. "send buffer overflow")
// into our Logger. AOO's default sink is stderr; routing it here keeps those
// messages in the log file alongside our own. Registered via AooSettings below.
void AOO_CALL aoo_log_forward(AooLogLevel level, const AooChar* msg) {
    if (!g_log || !msg) return;
    const char* lvl = (level == kAooLogLevelError)   ? "error"
                    : (level == kAooLogLevelWarning) ? "warn"
                                                     : "info";
    g_log->info(std::string("aoo[") + lvl + "]: " + msg);
}

// AOO event handler (poll mode; invoked from the net thread via pollEvents).
void AOO_CALL on_event(void* /*user*/, const AooEvent* event, AooThreadLevel) {
    switch (event->type) {
    case kAooEventStreamStart: {
        const AooEndpoint& ep = event->streamStart.endpoint; // remember the source
        if (ep.address && ep.addrlen <= sizeof(g_src_ss)) {
            std::memcpy(&g_src_ss, ep.address, ep.addrlen);
            g_src_len = ep.addrlen;
            g_src_id  = ep.id;
            g_have_src = true;
        }
        g_log->info("aoo: stream start");
        break;
    }
    case kAooEventStreamStop:    g_log->info("aoo: stream stop"); break;
    case kAooEventFormatChange:  g_log->info("aoo: format change"); break;
    case kAooEventBufferUnderrun:g_log->info("aoo: buffer UNDERRUN"); break;
    case kAooEventBufferOverrun: g_log->info("aoo: buffer OVERRUN"); break;
    case kAooEventBlockDrop:     g_log->info("aoo: block dropped (lost, unrecovered)"); break;
    case kAooEventBlockXRun:     g_log->info("aoo: block xrun"); break;
    default: break; // resend/ping/etc. — not logged
    }
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
    g_log = &log;
    log.info("receiver: starting (port=" + std::to_string(cfg.listen_port) + " device='" +
             cfg.device_name + "')");
    std::fprintf(stderr, "AudioPipe receiver (AOO): port %u -> device '%s'\n", cfg.listen_port,
                 cfg.device_name.c_str());

    AooSettings settings;                 // C++ default ctor fills structSize + nulls
    settings.logFunc = aoo_log_forward;   // route AOO's internal log into our Logger
    if (aoo_initialize(&settings) != kAooOk) {
        std::fprintf(stderr, "aoo_initialize failed\n");
        return 1;
    }

    const AudioDeviceID dev = find_output_device(cfg.device_name);
    if (dev == kAudioObjectUnknown) {
        std::fprintf(stderr, "device '%s' not found (is the driver installed?)\n",
                     cfg.device_name.c_str());
        log.info("receiver: device not found");
        return 1;
    }

    request_buffer_frames(dev, 128); // low-latency request; device may clamp
    const UInt32 dev_buf = device_buffer_frames(dev);

    // --- AOO sink ---------------------------------------------------------
    // Playout latency (jitter cushion) in seconds, from config.latency_ms.
    // AUDIOPIPE_LATENCY_MS overrides it for quick tuning without a rebuild.
    double latency_s = double(cfg.latency_ms) / 1000.0;
    if (const char* env = std::getenv("AUDIOPIPE_LATENCY_MS")) { // quick tuning override
        latency_s = std::atof(env) / 1000.0;
    }
    AooSink::Ptr sink = AooSink::create(kSinkId);
    sink->setup(cfg.channels, AooSampleRate(cfg.sample_rate), kMaxBlock, 0);
    // Buffer capacity is decoupled from playout latency: keep latency low (the
    // delay you hear) but give the network packet buffer generous headroom to
    // absorb scheduling/arrival bursts without overrunning. Default 4x latency
    // (min 80 ms); override with AUDIOPIPE_BUFFER_MS.
    double buffer_s = std::max(latency_s * 4.0, 0.08);
    if (const char* env = std::getenv("AUDIOPIPE_BUFFER_MS")) {
        buffer_s = std::atof(env) / 1000.0;
    }
    sink->setLatency(latency_s);
    sink->setBufferSize(buffer_s);
    sink->setResendData(kAooTrue);          // ask source to resend lost packets (low-RTT LAN)
    sink->setDynamicResampling(kAooTrue);   // time-DLL drift correction: track the device
                                            // clock vs the source clock and resample to match
                                            // (otherwise the small offset overruns the buffer)
    sink->setEventHandler(on_event, nullptr, kAooEventModePoll);

    RenderCtx ctx;
    ctx.sink     = sink.get();
    ctx.channels = cfg.channels;
    ctx.planar.assign(cfg.channels, std::vector<float>(kMaxBlock, 0.0f));
    ctx.ptrs.assign(cfg.channels, nullptr);

    // --- HAL output unit on the AudioPipe device --------------------------
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

    AURenderCallbackStruct cb = {render_cb, &ctx};
    AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb,
                         sizeof(cb));

    if (AudioUnitInitialize(unit) != noErr || AudioOutputUnitStart(unit) != noErr) {
        std::fprintf(stderr, "failed to start audio unit\n");
        return 1;
    }
    log.info("receiver: audio unit started (dev_buf=" + std::to_string(dev_buf) +
             " latency_ms=" + std::to_string(int(latency_s * 1000)) + ")");

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // --- Network thread: recvfrom -> sink; sink -> send; poll events ------
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::fprintf(stderr, "socket() failed\n");
        return 1;
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
        std::fprintf(stderr, "bind(%u) failed\n", cfg.listen_port);
        return 1;
    }

    std::thread net([&] {
        std::vector<AooByte> buf(AOO_MAX_PACKET_SIZE);
        std::uint64_t        pkts = 0, last_log = 0;
        while (!g_stop.load(std::memory_order_acquire)) {
            sockaddr_storage src{};
            socklen_t        srclen = sizeof(src);
            const ssize_t n = ::recvfrom(fd, buf.data(), buf.size(), 0,
                                         reinterpret_cast<sockaddr*>(&src), &srclen);
            if (n > 0) {
                sink->handleMessage(buf.data(), AooInt32(n), &src, srclen);
                ++pkts;
            }
            sink->send(udp_send, &fd);
            sink->pollEvents();

            // Sample the jitter-buffer fill ratio (0=empty, 1=full) to expose
            // how widely it swings; ~1.0 means an overrun is imminent.
            if (g_have_src) {
                AooEndpoint ep{&g_src_ss, g_src_len, g_src_id};
                double ratio = 0.0;
                if (sink->getBufferFillRatio(ep, ratio) == kAooOk) {
                    g_fill_min = std::min(g_fill_min, ratio);
                    g_fill_max = std::max(g_fill_max, ratio);
                }
            }
            if (pkts - last_log >= 2400) { // ~5 s heartbeat at 2 ms blocks
                AooSampleRate sr = 0.0;
                sink->getRealSampleRate(sr);
                char line[160];
                std::snprintf(line, sizeof line,
                              "aoo: buffer fill swing %.2f..%.2f  measured device rate %.1f Hz",
                              g_fill_min, g_fill_max, double(sr));
                log.info(line);
                g_fill_min = 1e9;
                g_fill_max = -1e9;
                last_log = pkts;
            }
        }
    });

    net.join();
    ::close(fd);

    AudioOutputUnitStop(unit);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    sink.reset();
    aoo_terminate();
    log.info("receiver: stopped");
    std::fprintf(stderr, "receiver: stopped\n");
    return 0;
}
