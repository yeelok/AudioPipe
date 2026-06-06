#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "audiopipe/jitter_buffer.h"

using namespace audiopipe;

namespace {

constexpr std::uint16_t kFrameSamples = 2;
constexpr std::uint16_t kChannels     = 2;
constexpr std::size_t   kFloats       = kFrameSamples * kChannels; // 4

// Deterministic, distinct contents per sequence number.
std::vector<float> make_frame(std::uint16_t seq) {
    std::vector<float> f(kFloats);
    for (std::size_t i = 0; i < kFloats; ++i) {
        f[i] = static_cast<float>(seq) * 100.0f + static_cast<float>(i);
    }
    return f;
}

void insert(JitterBuffer& jb, std::uint16_t seq) {
    const auto f = make_frame(seq);
    jb.insert(seq, f.data(), f.size());
}

} // namespace

TEST_CASE("in-order inserts play back bit-identically", "[jitter]") {
    JitterBuffer jb(3, kFrameSamples, kChannels);
    constexpr int kCount = 10;
    for (std::uint16_t s = 0; s < kCount; ++s) {
        insert(jb, s);
    }
    std::vector<float> out(kFloats);
    for (std::uint16_t s = 0; s < kCount; ++s) {
        CHECK(jb.pop_frame(out.data()));
        CHECK(out == make_frame(s));
    }
    CHECK(jb.lost_count() == 0);
    CHECK(jb.dropped_late_count() == 0);
}

TEST_CASE("reordered arrivals are corrected to sequence order", "[jitter]") {
    JitterBuffer jb(3, kFrameSamples, kChannels);
    for (std::uint16_t s : {2, 0, 1, 3, 5, 4, 6, 8, 7, 9}) {
        insert(jb, s);
    }
    std::vector<float> out(kFloats);
    for (std::uint16_t s = 0; s < 10; ++s) {
        CHECK(jb.pop_frame(out.data()));
        CHECK(out == make_frame(s));
    }
    CHECK(jb.lost_count() == 0);
    CHECK(jb.dropped_late_count() == 0);
}

TEST_CASE("missing frame is concealed by repeating the last good frame", "[jitter]") {
    JitterBuffer jb(3, kFrameSamples, kChannels);
    // Insert everything except sequence 5.
    for (std::uint16_t s = 0; s < 10; ++s) {
        if (s != 5) {
            insert(jb, s);
        }
    }
    std::vector<float> out(kFloats);
    for (std::uint16_t s = 0; s < 10; ++s) {
        const bool real = jb.pop_frame(out.data());
        if (s == 5) {
            CHECK_FALSE(real);
            CHECK(out == make_frame(4)); // repeat of last good (§8.2)
        } else {
            CHECK(real);
            CHECK(out == make_frame(s));
        }
    }
    CHECK(jb.lost_count() == 1);
}

TEST_CASE("packets older than the playout position are dropped", "[jitter]") {
    JitterBuffer jb(1, kFrameSamples, kChannels); // prime after 1 frame
    insert(jb, 0);
    std::vector<float> out(kFloats);
    REQUIRE(jb.pop_frame(out.data())); // plays seq 0, playout now at 1

    insert(jb, 0); // stale — older than playout
    CHECK(jb.dropped_late_count() == 1);

    // seq 1 never arrived -> concealed.
    CHECK_FALSE(jb.pop_frame(out.data()));
    CHECK(jb.lost_count() == 1);
}

TEST_CASE("playout is held with silence until the priming depth is reached", "[jitter]") {
    JitterBuffer jb(3, kFrameSamples, kChannels);
    std::vector<float> out(kFloats);
    const std::vector<float> silence(kFloats, 0.0f);

    insert(jb, 0);
    insert(jb, 1); // only 2 buffered, depth is 3
    CHECK_FALSE(jb.pop_frame(out.data()));
    CHECK(out == silence); // priming silence, no advance

    insert(jb, 2); // depth reached
    CHECK(jb.pop_frame(out.data()));
    CHECK(out == make_frame(0));
    CHECK(jb.pop_frame(out.data()));
    CHECK(out == make_frame(1));
    CHECK(jb.pop_frame(out.data()));
    CHECK(out == make_frame(2));

    // Buffer now empty -> underrun concealed with last good (frame 2).
    CHECK_FALSE(jb.pop_frame(out.data()));
    CHECK(out == make_frame(2));
    CHECK(jb.lost_count() == 1);
}
