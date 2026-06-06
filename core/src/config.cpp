#include "audiopipe/config.h"

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace audiopipe {
namespace {

nlohmann::json read_json_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("config: cannot open file: " + path);
    }
    try {
        nlohmann::json j;
        in >> j;
        return j;
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error("config: parse error in " + path + ": " + e.what());
    }
}

void require(const nlohmann::json& j, const char* key) {
    if (!j.contains(key)) {
        throw std::runtime_error(std::string("config: missing required field: ") + key);
    }
}

void validate_frame_samples(std::uint16_t n) {
    if (n < kMinFrameSamples || n > kMaxFrameSamples) {
        throw std::runtime_error("config: frame_samples out of range [48, 240]");
    }
}

} // namespace

SenderConfig load_sender_config(const std::string& path) {
    const auto j = read_json_file(path);
    require(j, "dest_ip");
    require(j, "dest_port");

    SenderConfig c;
    c.dest_ip          = j.at("dest_ip").get<std::string>();
    c.dest_port        = j.at("dest_port").get<std::uint16_t>();
    c.sample_rate      = j.value("sample_rate", c.sample_rate);
    c.channels         = j.value("channels", c.channels);
    c.frame_samples    = j.value("frame_samples", c.frame_samples);
    c.frames_per_packet = j.value("frames_per_packet", c.frames_per_packet);
    c.log_file         = j.value("log_file", std::string{});

    validate_frame_samples(c.frame_samples);
    return c;
}

ReceiverConfig load_receiver_config(const std::string& path) {
    const auto j = read_json_file(path);
    require(j, "listen_port");

    ReceiverConfig c;
    c.listen_port   = j.at("listen_port").get<std::uint16_t>();
    c.sample_rate   = j.value("sample_rate", c.sample_rate);
    c.channels      = j.value("channels", c.channels);
    c.jitter_frames = j.value("jitter_frames", c.jitter_frames);
    c.device_name   = j.value("device_name", c.device_name);
    c.log_file      = j.value("log_file", std::string{});
    return c;
}

} // namespace audiopipe
