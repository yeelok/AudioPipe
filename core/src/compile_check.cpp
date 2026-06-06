// Guarantees every public core header parses cleanly at M0, before its
// implementation lands. Per-milestone implementations replace the TODOs in the
// individual headers; this file just keeps the interfaces honest.

#include "audiopipe/config.h"
#include "audiopipe/drift_control.h"
#include "audiopipe/format.h"
#include "audiopipe/jitter_buffer.h"
#include "audiopipe/log.h"
#include "audiopipe/packet.h"
#include "audiopipe/resampler.h"
#include "audiopipe/ring_buffer.h"

namespace audiopipe {
namespace {

// Force the header-only template to instantiate so it is syntax-checked.
[[maybe_unused]] void header_smoke() {
    SpscRingBuffer<float> rb(8);
    (void)rb;
    static_assert(sizeof(PacketHeader) == 18, "wire header size");
}

} // namespace
} // namespace audiopipe
