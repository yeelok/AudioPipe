#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <string>

#include "audiopipe/config.h"

using namespace audiopipe;

namespace {

// Writes `content` to a temp file in the build dir and returns its path.
std::string write_temp(const std::string& name, const std::string& content) {
    const std::string path = "./" + name;
    std::ofstream(path) << content;
    return path;
}

} // namespace

TEST_CASE("sender config: explicit values override defaults", "[config]") {
    const auto path = write_temp("sender_test.json", R"({
        "dest_ip": "192.168.1.50",
        "dest_port": 4010,
        "frame_samples": 48,
        "frames_per_packet": 2,
        "log_file": "/tmp/sender.log"
    })");
    const auto c = load_sender_config(path);
    CHECK(c.dest_ip == "192.168.1.50");
    CHECK(c.dest_port == 4010);
    CHECK(c.frame_samples == 48);
    CHECK(c.frames_per_packet == 2);
    CHECK(c.sample_rate == kSampleRate); // defaulted
    CHECK(c.channels == kChannels);      // defaulted
    std::remove(path.c_str());
}

TEST_CASE("sender config: missing required field throws", "[config]") {
    const auto path = write_temp("sender_bad.json", R"({ "dest_port": 4010 })");
    CHECK_THROWS(load_sender_config(path));
    std::remove(path.c_str());
}

TEST_CASE("sender config: out-of-range frame size throws", "[config]") {
    const auto path = write_temp("sender_oor.json", R"({
        "dest_ip": "10.0.0.2", "dest_port": 4010, "frame_samples": 16
    })");
    CHECK_THROWS(load_sender_config(path));
    std::remove(path.c_str());
}

TEST_CASE("receiver config: defaults applied for omitted fields", "[config]") {
    const auto path = write_temp("receiver_test.json", R"({ "listen_port": 4010 })");
    const auto c = load_receiver_config(path);
    CHECK(c.listen_port == 4010);
    CHECK(c.jitter_frames == 3);
    CHECK(c.sample_rate == kSampleRate);
    CHECK(c.channels == kChannels);
    std::remove(path.c_str());
}
