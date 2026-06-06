#include "audiopipe/packet.h"

#include <cstring>

namespace audiopipe {
namespace {

// --- Little-endian primitives (portable on any host byte order) ------------

void put_u16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}

void put_u32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

void put_u64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

std::uint16_t get_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) | static_cast<std::uint16_t>(p[1] << 8);
}

std::uint32_t get_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t get_u64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

void put_f32(std::uint8_t* p, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    put_u32(p, bits);
}

float get_f32(const std::uint8_t* p) {
    std::uint32_t bits = get_u32(p);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

} // namespace

std::size_t encode(const PacketHeader& hdr, const float* samples, std::size_t sample_count,
                   std::uint8_t* dst, std::size_t dst_cap) {
    const std::size_t total = packet_size(sample_count);
    if (dst_cap < total) {
        return 0;
    }

    put_u32(dst + 0, hdr.magic);
    put_u16(dst + 4, hdr.sequence);
    put_u64(dst + 6, hdr.timestamp);
    put_u16(dst + 14, hdr.frame_count);
    dst[16] = hdr.channels;
    dst[17] = hdr.format_id;

    std::uint8_t* p = dst + sizeof(PacketHeader);
    for (std::size_t i = 0; i < sample_count; ++i) {
        put_f32(p, samples[i]);
        p += kBytesPerSample;
    }
    return total;
}

bool decode_header(const std::uint8_t* src, std::size_t len, PacketHeader& out) {
    if (len < sizeof(PacketHeader)) {
        return false;
    }
    out.magic       = get_u32(src + 0);
    out.sequence    = get_u16(src + 4);
    out.timestamp   = get_u64(src + 6);
    out.frame_count = get_u16(src + 14);
    out.channels    = src[16];
    out.format_id   = src[17];

    if (out.magic != kMagic) {
        return false;
    }
    if (out.format_id != static_cast<std::uint8_t>(FormatId::Float32)) {
        return false;
    }
    return true;
}

std::size_t decode_payload(const std::uint8_t* src, std::size_t len, const PacketHeader& hdr,
                           float* out, std::size_t out_cap) {
    const std::size_t floats = static_cast<std::size_t>(hdr.frame_count) * hdr.channels;
    if (len < packet_size(floats) || out_cap < floats) {
        return 0;
    }
    const std::uint8_t* p = src + sizeof(PacketHeader);
    for (std::size_t i = 0; i < floats; ++i) {
        out[i] = get_f32(p);
        p += kBytesPerSample;
    }
    return floats;
}

} // namespace audiopipe
