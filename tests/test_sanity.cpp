#include <catch2/catch_test_macros.hpp>

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

// The wire protocol (formerly the §7 PacketHeader) is now owned by AOO
// (deps/aoo); its framing is covered by AOO's own tests and by
// test_aoo_loopback.cpp.
