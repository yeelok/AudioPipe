#include <catch2/catch_test_macros.hpp>

#include <cstddef>

#include "audiopipe/format.h"

using namespace audiopipe;

TEST_CASE("fixed audio format matches spec §4", "[format]") {
    CHECK(kSampleRate == 48000);
    CHECK(kChannels == 2);
    CHECK(kBytesPerSample == 4);
    CHECK(kDefaultFrameSamples == 96);
    CHECK(kMinFrameSamples == 48);
    CHECK(kMaxFrameSamples == 240);
}

TEST_CASE("packet header wire layout is 18 bytes with no padding", "[format][wire]") {
    STATIC_REQUIRE(sizeof(PacketHeader) == 18);
    // Field offsets must match the §7 byte order exactly.
    CHECK(offsetof(PacketHeader, magic) == 0);
    CHECK(offsetof(PacketHeader, sequence) == 4);
    CHECK(offsetof(PacketHeader, timestamp) == 6);
    CHECK(offsetof(PacketHeader, frame_count) == 14);
    CHECK(offsetof(PacketHeader, channels) == 16);
    CHECK(offsetof(PacketHeader, format_id) == 17);
}

TEST_CASE("payload_bytes computes interleaved float32 size", "[format]") {
    CHECK(payload_bytes(96, 2) == 96u * 2u * 4u);
    CHECK(payload_bytes(kDefaultFrameSamples) == 768u); // default frame, stereo
}
