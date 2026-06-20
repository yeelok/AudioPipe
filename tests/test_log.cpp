// Logger housekeeping: the file is size-rotated so an always-on service can't
// grow it without bound. These tests force many small rotations and check the
// active file stays under the cap and the number of backups is bounded by `keep`.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <string>

#include "audiopipe/log.h"

using namespace audiopipe;

namespace {
bool exists(const std::string& p) { std::ifstream f(p); return static_cast<bool>(f); }
std::size_t fsize(const std::string& p) {
    std::ifstream f(p, std::ios::ate | std::ios::binary);
    return f ? static_cast<std::size_t>(f.tellg()) : 0;
}
void cleanup(const std::string& base, int upto) {
    std::remove(base.c_str());
    for (int i = 1; i <= upto; ++i) std::remove((base + "." + std::to_string(i)).c_str());
}
} // namespace

TEST_CASE("Logger size-rotates and bounds total log size", "[log]") {
    const std::string base = "audiopipe_test_log.tmp";
    const int         keep = 2;
    const std::size_t cap  = 512; // tiny cap to force frequent rotation
    cleanup(base, keep + 2);

    {
        Logger log(base, cap, keep);
        for (int i = 0; i < 500; ++i) {
            log.info("rotation test line with some padding so it counts xxxxxxxxxx");
        }
    } // close the file

    REQUIRE(exists(base));
    CHECK(fsize(base) <= cap);              // active file never exceeds the cap
    CHECK(exists(base + ".1"));             // rotated backups exist...
    CHECK(exists(base + ".2"));
    CHECK_FALSE(exists(base + ".3"));       // ...but never more than `keep`

    cleanup(base, keep + 2);
}

TEST_CASE("Logger with max_bytes=0 disables rotation", "[log]") {
    const std::string base = "audiopipe_test_log_norotate.tmp";
    cleanup(base, 3);

    {
        Logger log(base, 0, 3); // rotation off → unbounded single file
        for (int i = 0; i < 200; ++i) log.info("line line line line line line line");
    }

    REQUIRE(exists(base));
    CHECK_FALSE(exists(base + ".1"));       // never rotated

    cleanup(base, 3);
}

TEST_CASE("Logger appends across reopen (rotation seeded from existing size)", "[log]") {
    const std::string base = "audiopipe_test_log_append.tmp";
    cleanup(base, 3);

    { Logger a(base, 0, 0); a.info("first session line"); }
    const std::size_t after_first = fsize(base);
    { Logger b(base, 0, 0); b.info("second session line"); }
    CHECK(fsize(base) > after_first);       // appended, not truncated

    cleanup(base, 3);
}
