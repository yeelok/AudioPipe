#include "sender_app.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>

#include "audiopipe/format.h"
#include "audiopipe/packet.h"
#include "udp_sender.h"
#include "wasapi_capture.h"

namespace audiopipe {
namespace win {

int run_sender(const SenderConfig& cfg, std::atomic<bool>& stop, Logger& log) {
    UdpSender   net;
    std::string err;
    if (!net.open(cfg.dest_ip, cfg.dest_port, err)) {
        log.info("sender: net open failed: " + err);
        return 1;
    }

    // Samples-per-channel carried in each packet (one or more frames; §7).
    const std::uint16_t fpp = cfg.frames_per_packet ? cfg.frames_per_packet : 1;
    const std::uint16_t packet_spc = static_cast<std::uint16_t>(cfg.frame_samples * fpp);

    WasapiLoopbackCapture cap;
    bool                  opened = false;
    std::vector<float>    accum;
    std::vector<std::uint8_t> datagram;
    std::uint16_t         seq = 0;
    std::uint64_t         ts  = 0;
    std::uint64_t         sent = 0;

    const std::string dev_label =
        cfg.device_name.empty() ? std::string("default render device") : cfg.device_name;

    while (!stop.load(std::memory_order_acquire)) {
        if (!opened) {
            if (cap.open(cfg.device_name, err)) {
                opened = true;
                log.info("sender: capturing '" + dev_label + "' at " +
                         std::to_string(cap.sample_rate()) + " Hz, " +
                         std::to_string(cap.channels()) + "ch");
                if (cap.sample_rate() != cfg.sample_rate) {
                    log.info("sender: WARNING device rate " + std::to_string(cap.sample_rate()) +
                             " != config sample_rate " + std::to_string(cfg.sample_rate));
                }
            } else {
                log.info("sender: capture open failed (" + err + "); retrying in 1s");
                Sleep(1000);
                continue;
            }
        }

        bool silent = false;
        if (!cap.capture(accum, silent, err)) {
            log.info("sender: capture error (" + err + "); reopening");
            cap.close();
            opened = false;
            Sleep(500);
            continue;
        }

        const std::uint16_t ch            = cap.channels();
        const std::size_t   packet_floats = std::size_t(packet_spc) * ch;
        const std::size_t   need          = packet_size(packet_floats);
        if (datagram.size() < need) {
            datagram.resize(need);
        }

        std::size_t consumed = 0;
        while (accum.size() - consumed >= packet_floats) {
            PacketHeader h{};
            h.magic       = kMagic;
            h.sequence    = seq++;
            h.timestamp   = ts;
            h.frame_count = packet_spc;
            h.channels    = static_cast<std::uint8_t>(ch);
            h.format_id   = static_cast<std::uint8_t>(FormatId::Float32);
            ts += packet_spc;

            encode(h, &accum[consumed], packet_floats, datagram.data(), datagram.size());
            net.send(datagram.data(), need);
            consumed += packet_floats;
            ++sent;
        }
        if (consumed > 0) {
            accum.erase(accum.begin(), accum.begin() + consumed);
        }

        // Light poll: capture() returns immediately, so sleep ~half a frame to
        // avoid a busy spin while still keeping latency low.
        const DWORD half_frame_ms =
            static_cast<DWORD>((1000.0 * cfg.frame_samples) / (2.0 * cfg.sample_rate));
        Sleep(half_frame_ms > 0 ? half_frame_ms : 1);
    }

    cap.close();
    net.close();
    log.info("sender: stopped, packets sent=" + std::to_string(sent));
    return 0;
}

} // namespace win
} // namespace audiopipe
