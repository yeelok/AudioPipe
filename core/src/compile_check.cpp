// Guarantees the remaining public core headers parse cleanly. The transport
// modules (packet/jitter/drift/resampler) were removed when AudioPipe adopted
// the AOO library (deps/aoo) for the send/receive path; what's left in core/ is
// config, logging, the audio-format constants, and a general lock-free ring.

#include "audiopipe/config.h"
#include "audiopipe/format.h"
#include "audiopipe/log.h"
#include "audiopipe/ring_buffer.h"

namespace audiopipe {
namespace {

// Force the header-only template to instantiate so it is syntax-checked.
[[maybe_unused]] void header_smoke() {
    SpscRingBuffer<float> rb(8);
    (void)rb;
    static_assert(kSampleRate == 48'000, "fixed 48 kHz format");
}

} // namespace
} // namespace audiopipe
