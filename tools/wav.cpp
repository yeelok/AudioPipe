#include "wav.h"

#include <cstring>
#include <fstream>

namespace audiopipe {
namespace tools {
namespace {

void put_u16(std::ofstream& o, std::uint16_t v) {
    const std::uint8_t b[2] = {std::uint8_t(v), std::uint8_t(v >> 8)};
    o.write(reinterpret_cast<const char*>(b), 2);
}

void put_u32(std::ofstream& o, std::uint32_t v) {
    const std::uint8_t b[4] = {std::uint8_t(v), std::uint8_t(v >> 8), std::uint8_t(v >> 16),
                               std::uint8_t(v >> 24)};
    o.write(reinterpret_cast<const char*>(b), 4);
}

std::uint16_t rd_u16(const std::uint8_t* p) {
    return std::uint16_t(p[0] | (p[1] << 8));
}

std::uint32_t rd_u32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}

} // namespace

bool write_wav_f32(const std::string& path, const WavData& w) {
    std::ofstream o(path, std::ios::binary);
    if (!o) {
        return false;
    }
    const std::uint16_t bits        = 32;
    const std::uint16_t block_align = std::uint16_t(w.channels * (bits / 8));
    const std::uint32_t byte_rate   = w.sample_rate * block_align;
    const std::uint32_t data_bytes  = std::uint32_t(w.samples.size() * sizeof(float));

    o.write("RIFF", 4);
    put_u32(o, 36 + data_bytes);
    o.write("WAVE", 4);
    o.write("fmt ", 4);
    put_u32(o, 16);
    put_u16(o, 3); // WAVE_FORMAT_IEEE_FLOAT
    put_u16(o, w.channels);
    put_u32(o, w.sample_rate);
    put_u32(o, byte_rate);
    put_u16(o, block_align);
    put_u16(o, bits);
    o.write("data", 4);
    put_u32(o, data_bytes);
    o.write(reinterpret_cast<const char*>(w.samples.data()),
            static_cast<std::streamsize>(data_bytes));
    return static_cast<bool>(o);
}

bool read_wav(const std::string& path, WavData& out, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "cannot open " + path;
        return false;
    }
    std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    if (raw.size() < 12 || std::memcmp(raw.data(), "RIFF", 4) != 0 ||
        std::memcmp(raw.data() + 8, "WAVE", 4) != 0) {
        err = "not a RIFF/WAVE file";
        return false;
    }

    std::uint16_t fmt = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0;
    const std::uint8_t* data = nullptr;
    std::uint32_t data_size = 0;

    std::size_t pos = 12;
    while (pos + 8 <= raw.size()) {
        const std::uint8_t* id = raw.data() + pos;
        const std::uint32_t sz = rd_u32(raw.data() + pos + 4);
        const std::size_t body = pos + 8;
        if (std::memcmp(id, "fmt ", 4) == 0 && body + 16 <= raw.size()) {
            fmt      = rd_u16(raw.data() + body + 0);
            channels = rd_u16(raw.data() + body + 2);
            rate     = rd_u32(raw.data() + body + 4);
            bits     = rd_u16(raw.data() + body + 14);
        } else if (std::memcmp(id, "data", 4) == 0) {
            data      = raw.data() + body;
            data_size = std::min<std::uint32_t>(sz, std::uint32_t(raw.size() - body));
        }
        pos = body + sz + (sz & 1); // chunks are word-aligned
    }

    if (!data || channels == 0) {
        err = "missing fmt/data chunk";
        return false;
    }

    out.sample_rate = rate;
    out.channels    = channels;
    out.samples.clear();

    if (fmt == 3 && bits == 32) {
        const std::size_t n = data_size / 4;
        out.samples.resize(n);
        std::memcpy(out.samples.data(), data, n * 4);
    } else if (fmt == 1 && bits == 16) {
        const std::size_t n = data_size / 2;
        out.samples.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const std::int16_t s = std::int16_t(rd_u16(data + i * 2));
            out.samples[i] = float(s) / 32768.0f;
        }
    } else {
        err = "unsupported WAV format (need 16-bit PCM or 32-bit float)";
        return false;
    }
    return true;
}

} // namespace tools
} // namespace audiopipe
