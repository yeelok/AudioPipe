#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace audiopipe {
namespace win {

// WASAPI shared-mode loopback capture of a render endpoint (spec §5.2). Audio
// rendered to the device (e.g. a per-app stream routed to the AudioPipe virtual
// device) is captured as interleaved float32 at the device mix rate/channels.
// Windows headers are kept out of this header via PIMPL.
class WasapiLoopbackCapture {
public:
    WasapiLoopbackCapture();
    ~WasapiLoopbackCapture();
    WasapiLoopbackCapture(const WasapiLoopbackCapture&)            = delete;
    WasapiLoopbackCapture& operator=(const WasapiLoopbackCapture&) = delete;

    // Open the active render device whose friendly name contains
    // `device_substr` (empty = default render endpoint). Fills the format on
    // success; returns false and sets `err` otherwise.
    bool open(const std::string& device_substr, std::string& err);

    // Append all currently-available captured samples to `out`. Sets `silent`
    // if any captured region was flagged silent. Returns false on a fatal
    // error (the caller may close() and re-open()).
    bool capture(std::vector<float>& out, bool& silent, std::string& err);

    void close();

    std::uint32_t sample_rate() const;
    std::uint16_t channels() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace win
} // namespace audiopipe
