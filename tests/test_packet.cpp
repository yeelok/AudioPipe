#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include "audiopipe/format.h"
#include "audiopipe/packet.h"

using namespace audiopipe;

namespace {

PacketHeader make_header(std::uint16_t seq, std::uint16_t frames) {
    PacketHeader h{};
    h.magic       = kMagic;
    h.sequence    = seq;
    h.timestamp   = 0x0102030405060708ull;
    h.frame_count = frames;
    h.channels    = static_cast<std::uint8_t>(kChannels);
    h.format_id   = static_cast<std::uint8_t>(FormatId::Float32);
    return h;
}

} // namespace

TEST_CASE("encode/decode round-trip preserves header and samples", "[packet]") {
    const std::vector<float> samples = {0.0f, 1.0f, -1.0f, 0.25f, -0.5f, 123.456f};
    const auto hdr = make_header(42, 3); // 3 frames * 2ch == 6 floats

    std::vector<std::uint8_t> buf(packet_size(samples.size()));
    const auto written = encode(hdr, samples.data(), samples.size(), buf.data(), buf.size());
    REQUIRE(written == buf.size());
    REQUIRE(written == sizeof(PacketHeader) + samples.size() * 4);

    PacketHeader out{};
    REQUIRE(decode_header(buf.data(), buf.size(), out));
    CHECK(out.magic == kMagic);
    CHECK(out.sequence == 42);
    CHECK(out.timestamp == 0x0102030405060708ull);
    CHECK(out.frame_count == 3);
    CHECK(out.channels == kChannels);

    std::vector<float> got(samples.size());
    REQUIRE(decode_payload(buf.data(), buf.size(), out, got.data(), got.size()) == samples.size());
    CHECK(got == samples);
}

TEST_CASE("integer fields and floats serialize little-endian", "[packet][wire]") {
    const std::array<float, 2> samples = {1.0f, 0.0f};
    const auto hdr = make_header(0x3412, 1);

    std::vector<std::uint8_t> buf(packet_size(samples.size()));
    REQUIRE(encode(hdr, samples.data(), samples.size(), buf.data(), buf.size()) == buf.size());

    // magic 0x314C5041 -> bytes 41 50 4C 31
    CHECK(buf[0] == 0x41);
    CHECK(buf[1] == 0x50);
    CHECK(buf[2] == 0x4C);
    CHECK(buf[3] == 0x31);
    // sequence 0x3412 -> bytes 12 34
    CHECK(buf[4] == 0x12);
    CHECK(buf[5] == 0x34);
    // first sample 1.0f -> IEEE754 0x3F800000 -> LE bytes 00 00 80 3F
    const std::size_t off = sizeof(PacketHeader);
    CHECK(buf[off + 0] == 0x00);
    CHECK(buf[off + 1] == 0x00);
    CHECK(buf[off + 2] == 0x80);
    CHECK(buf[off + 3] == 0x3F);
}

TEST_CASE("sequence number round-trips across the wrap boundary", "[packet]") {
    for (std::uint16_t seq : {std::uint16_t(0), std::uint16_t(65535), std::uint16_t(65534)}) {
        const std::array<float, 2> s = {0.5f, -0.5f};
        const auto hdr = make_header(seq, 1);
        std::vector<std::uint8_t> buf(packet_size(s.size()));
        REQUIRE(encode(hdr, s.data(), s.size(), buf.data(), buf.size()) == buf.size());
        PacketHeader out{};
        REQUIRE(decode_header(buf.data(), buf.size(), out));
        CHECK(out.sequence == seq);
    }
}

TEST_CASE("decode rejects truncated and invalid input", "[packet]") {
    const std::array<float, 2> s = {1.0f, 2.0f};
    const auto hdr = make_header(1, 1);
    std::vector<std::uint8_t> buf(packet_size(s.size()));
    REQUIRE(encode(hdr, s.data(), s.size(), buf.data(), buf.size()) == buf.size());

    PacketHeader out{};
    // Header truncated.
    CHECK_FALSE(decode_header(buf.data(), sizeof(PacketHeader) - 1, out));
    // Payload truncated (header ok, body short).
    REQUIRE(decode_header(buf.data(), buf.size(), out));
    std::array<float, 2> got{};
    CHECK(decode_payload(buf.data(), buf.size() - 1, out, got.data(), got.size()) == 0);
    // Bad magic.
    buf[0] ^= 0xFF;
    CHECK_FALSE(decode_header(buf.data(), buf.size(), out));
}

TEST_CASE("encode reports overflow when the buffer is too small", "[packet]") {
    const std::array<float, 2> s = {1.0f, 2.0f};
    const auto hdr = make_header(1, 1);
    std::vector<std::uint8_t> small(sizeof(PacketHeader)); // no room for payload
    CHECK(encode(hdr, s.data(), s.size(), small.data(), small.size()) == 0);
}
