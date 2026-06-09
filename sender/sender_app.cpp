#include "sender_app.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>  // SIO_UDP_CONNRESET
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>  // timeBeginPeriod/timeEndPeriod (winmm)

#ifndef SIO_UDP_CONNRESET            // declared in <mstcpip.h>, but guarded in some SDKs
#  define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <string>
#include <thread>
#include <vector>

#include "aoo.h"
#include "aoo_source.hpp"
#include "codec/aoo_pcm.h"

#include "audiopipe/format.h"
#include "wasapi_capture.h"

// NOTE: This file is built only on Windows (see sender/CMakeLists.txt) and is
// not compiled by the macOS CI on this machine. It mirrors the AOO usage that
// the receiver and tools/aoo_send.cpp were validated with on macOS.

namespace audiopipe {
namespace win {

namespace {

constexpr AooId kSourceId = 1;
constexpr AooId kSinkId   = 1;

Logger* g_log = nullptr;

// AOO -> UDP. AOO supplies the destination address (the sink we added).
AooInt32 AOO_CALL udp_send(void* user, const AooByte* data, AooInt32 size,
                           const void* address, AooAddrSize addrlen, AooFlag) {
    SOCKET s = *static_cast<SOCKET*>(user);
    const int sent = ::sendto(s, reinterpret_cast<const char*>(data), int(size), 0,
                              static_cast<const sockaddr*>(address), int(addrlen));
    // Non-blocking socket: if the send can't go out now (WSAEWOULDBLOCK) or errors,
    // drop this packet and report it as sent so AOO keeps draining its buffer instead
    // of stalling. Real-time -- stale audio is useless; the sink recovers any gap via
    // its jitter buffer / resend.
    return sent == SOCKET_ERROR ? size : AooInt32(sent);
}

// RAII: request 1 ms multimedia-timer resolution for the duration of a run.
// The capture poll loop and the net-thread pacing both Sleep(1); at Windows' default
// ~15.6 ms timer granularity those sleeps are far too coarse and the AOO source's
// send buffer overflows. Must be balanced with timeEndPeriod.
struct TimerResolutionGuard {
    TimerResolutionGuard()  { timeBeginPeriod(1); }
    ~TimerResolutionGuard() { timeEndPeriod(1); }
};

void AOO_CALL on_event(void*, const AooEvent* e, AooThreadLevel) {
    if (!g_log) return;
    switch (e->type) {
    case kAooEventSinkAdd:        g_log->info("aoo: sink added"); break;
    case kAooEventSinkRemove:     g_log->info("aoo: sink removed"); break;
    case kAooEventBufferOverrun:  g_log->info("aoo: sink buffer overrun"); break;
    case kAooEventBufferUnderrun: g_log->info("aoo: sink buffer underrun"); break;
    default: break;
    }
}

// Forward AOO's internal log (warnings/errors, e.g. "send buffer overflow") into
// our Logger. Critical here: a Windows service has no console, so AOO's default
// stderr sink is lost -- this keeps those messages in the log file.
void AOO_CALL aoo_log_forward(AooLogLevel level, const AooChar* msg) {
    if (!g_log || !msg) return;
    const char* lvl = (level == kAooLogLevelError)   ? "error"
                    : (level == kAooLogLevelWarning) ? "warn"
                                                     : "info";
    g_log->info(std::string("aoo[") + lvl + "]: " + msg);
}

} // namespace

int run_sender(const SenderConfig& cfg, std::atomic<bool>& stop, Logger& log) {
    g_log = &log;
    TimerResolutionGuard timer_res; // 1 ms timer resolution (see struct above)
    AooSettings settings;                 // C++ default ctor fills structSize + nulls
    settings.logFunc = aoo_log_forward;   // route AOO's log into our Logger (service has no stderr)
    if (aoo_initialize(&settings) != kAooOk) {
        log.info("sender: aoo_initialize failed");
        return 1;
    }

    // --- UDP socket + destination endpoint -------------------------------
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) {
        log.info("sender: socket() failed");
        aoo_terminate();
        return 1;
    }
    // Windows surfaces an ICMP "port unreachable" from the destination as
    // WSAECONNRESET on the next send/recv -- that permanently breaks this socket
    // (AOO can no longer drain its send buffer -> endless "send buffer overflow")
    // whenever the receiver is down/unreachable. Disable it so a missing receiver
    // is just silently-dropped UDP (spec 5.7: no crash if the network is down).
    BOOL  conn_reset  = FALSE;
    DWORD ioctl_bytes = 0;
    ::WSAIoctl(s, SIO_UDP_CONNRESET, &conn_reset, sizeof(conn_reset), nullptr, 0,
               &ioctl_bytes, nullptr, nullptr);
    // Non-blocking: the net thread must never stall in sendto (a stalled send backs
    // the stream up). sendto/recvfrom return at once; a momentarily full send buffer
    // drops the packet (see udp_send), and the thread paces itself with a 1 ms sleep
    // when there is nothing to receive.
    u_long nonblocking = 1;
    ::ioctlsocket(s, FIONBIO, &nonblocking);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(cfg.dest_port);
    if (::inet_pton(AF_INET, cfg.dest_ip.c_str(), &dst.sin_addr) != 1) {
        log.info("sender: bad dest_ip '" + cfg.dest_ip + "'");
        ::closesocket(s);
        aoo_terminate();
        return 1;
    }

    // --- Open WASAPI capture (retry until available or stopped) -----------
    const std::string dev_label =
        cfg.device_name.empty() ? std::string("default render device") : cfg.device_name;
    WasapiLoopbackCapture cap;
    std::string           err;
    while (!stop.load(std::memory_order_acquire)) {
        if (cap.open(cfg.device_name, err)) break;
        log.info("sender: capture open failed (" + err + "); retrying in 1s");
        Sleep(1000);
    }
    if (stop.load(std::memory_order_acquire)) {
        ::closesocket(s);
        aoo_terminate();
        return 0;
    }

    const std::uint16_t ch    = cap.channels();
    const std::uint32_t rate  = cap.sample_rate();
    const int           block = cfg.frame_samples ? cfg.frame_samples : kDefaultFrameSamples;
    log.info("sender: capturing '" + dev_label + "' at " + std::to_string(rate) + " Hz, " +
             std::to_string(ch) + "ch (AOO streams at this rate; the Mac sink resamples to " +
             std::to_string(cfg.sample_rate) + ")");

    // --- AOO source -------------------------------------------------------
    AooSource::Ptr source = AooSource::create(kSourceId);
    source->setEventHandler(on_event, nullptr, kAooEventModePoll);
    source->setup(ch, AooSampleRate(rate), block, 0);
    AooFormatPcm fmt;
    AooFormatPcm_init(&fmt, ch, AooInt32(rate), block, kAooPcmFloat32); // PCM format rate is AooInt32 (setup() above takes the double AooSampleRate)
    source->setFormat(fmt.header);
    source->setResendBufferSize(0.05);        // honor sink resend requests
    source->setBufferSize(0.1);               // 100 ms outgoing send queue (AOO default is 25 ms).
                                              // A WASAPI/scheduling burst that overruns this queue
                                              // makes process() return kAooErrorOverflow AND calls
                                              // add_xrun() -- which the sink reports as "block xrun".
                                              // This is headroom only (drains promptly), no added
                                              // latency. Root-cause fix for the overflow/xrun; the
                                              // self-heal stream-restart below stays as a safety net.
    source->setDynamicResampling(kAooTrue);    // measure the WASAPI capture clock
    AooEndpoint ep{ &dst, sizeof(dst), kSinkId };
    source->addSink(ep, kAooTrue);

    // --- Network thread: flush packets + handle resend requests -----------
    std::atomic<bool> net_stop{false};
    std::thread net([&] {
        std::vector<AooByte> buf(AOO_MAX_PACKET_SIZE);
        while (!net_stop.load(std::memory_order_acquire)) {
            source->send(udp_send, &s);
            sockaddr_storage from{};
            int              fromlen = sizeof(from);
            const int n = ::recvfrom(s, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                                     reinterpret_cast<sockaddr*>(&from), &fromlen);
            if (n > 0) {
                source->handleMessage(buf.data(), AooInt32(n), &from, AooAddrSize(fromlen));
            } else {
                ::Sleep(1); // non-blocking socket: pace the loop instead of busy-spinning
            }
            source->pollEvents();
        }
    });

    source->startStream(0, nullptr);

    // --- Capture -> process loop (audio thread) ---------------------------
    std::vector<float>              accum;   // interleaved float32 from WASAPI
    std::vector<std::vector<float>> planar(ch, std::vector<float>(block, 0.0f));
    std::vector<AooSample*>         ptrs(ch);
    const std::size_t               block_floats = std::size_t(block) * ch;

    std::uint64_t processed = 0, last_hb = 0;
    const std::uint64_t hb_blocks = rate ? (30u * rate) / std::uint32_t(block) : 15000u;
    // Self-heal: an AOO source stream-state stall (observed mid-session) makes the
    // send buffer overflow on *every* block and never drain, so audio stops until a
    // manual restart. Detect a sustained stall and force a fresh stream -- startStream
    // unconditionally resets the stream state (sequence + queue), so the sender (and
    // the starved sink) recover on their own.
    std::uint64_t consec_overflow = 0;
    const std::uint64_t stall_blocks = rate ? rate / (4u * std::uint32_t(block)) : 125u; // ~0.25 s

    while (!stop.load(std::memory_order_acquire)) {
        bool silent = false;
        if (!cap.capture(accum, silent, err)) {
            log.info("sender: capture error (" + err + "); stopping (service will restart)");
            break;
        }
        std::size_t consumed = 0;
        while (accum.size() - consumed >= block_floats) {
            for (int i = 0; i < block; ++i) {
                for (std::uint16_t c = 0; c < ch; ++c) {
                    planar[c][i] = accum[consumed + std::size_t(i) * ch + c];
                }
            }
            for (std::uint16_t c = 0; c < ch; ++c) ptrs[c] = planar[c].data();
            if (source->process(ptrs.data(), block, aoo_getCurrentNtpTime()) == kAooErrorOverflow) {
                if (++consec_overflow == stall_blocks) {
                    log.info("sender: AOO send buffer stalled (~" + std::to_string(stall_blocks) +
                             " blocks); restarting stream to recover");
                    source->startStream(0, nullptr);
                    consec_overflow = 0;
                }
            } else {
                consec_overflow = 0;
            }
            consumed += block_floats;
            ++processed;
        }
        if (consumed > 0) {
            accum.erase(accum.begin(), accum.begin() + consumed);
        }
        if (processed - last_hb >= hb_blocks) {
            log.info("sender: heartbeat blocks_sent=" + std::to_string(processed) +
                     (silent ? " (source silent)" : ""));
            last_hb = processed;
        }
        const DWORD half_block_ms = static_cast<DWORD>((1000.0 * block) / (2.0 * rate));
        Sleep(half_block_ms > 0 ? half_block_ms : 1);
    }

    // --- Teardown ---------------------------------------------------------
    source->stopStream(0);
    Sleep(50);                                   // let the net thread flush /stop
    net_stop.store(true, std::memory_order_release);
    if (net.joinable()) net.join();
    cap.close();
    source.reset();
    ::closesocket(s);
    aoo_terminate();
    log.info("sender: stopped, blocks sent=" + std::to_string(processed));
    return 0;
}

} // namespace win
} // namespace audiopipe
