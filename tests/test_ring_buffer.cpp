#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

#include "audiopipe/ring_buffer.h"

using namespace audiopipe;

TEST_CASE("push then pop preserves values and order", "[ring]") {
    SpscRingBuffer<int> rb(8);
    const std::vector<int> in = {10, 20, 30, 40};
    CHECK(rb.push(in.data(), in.size()) == in.size());
    CHECK(rb.size() == in.size());

    std::vector<int> out(in.size());
    CHECK(rb.pop(out.data(), out.size()) == in.size());
    CHECK(out == in);
    CHECK(rb.size() == 0);
}

TEST_CASE("push past capacity writes only what fits", "[ring]") {
    SpscRingBuffer<int> rb(4); // usable capacity == 4
    const std::vector<int> in = {1, 2, 3, 4, 5, 6};
    CHECK(rb.push(in.data(), in.size()) == 4);
    CHECK(rb.size() == 4);

    std::vector<int> out(10, -1);
    CHECK(rb.pop(out.data(), out.size()) == 4);
    CHECK((out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 4));
}

TEST_CASE("pop from empty returns zero", "[ring]") {
    SpscRingBuffer<int> rb(4);
    int x = 0;
    CHECK(rb.pop(&x, 1) == 0);
}

TEST_CASE("indices wrap correctly across the backing array", "[ring]") {
    SpscRingBuffer<int> rb(4);
    // Fill, drain partially, then write across the wrap point.
    const std::vector<int> a = {1, 2, 3};
    rb.push(a.data(), a.size());
    std::vector<int> tmp(2);
    rb.pop(tmp.data(), 2); // consumed 1,2 -> tail advanced
    const std::vector<int> b = {4, 5, 6};
    CHECK(rb.push(b.data(), b.size()) == 3); // wraps around the end
    std::vector<int> out(4);
    CHECK(rb.pop(out.data(), 4) == 4);
    CHECK((out[0] == 3 && out[1] == 4 && out[2] == 5 && out[3] == 6));
}

TEST_CASE("single-producer/single-consumer streams all items intact", "[ring][threads]") {
    constexpr int kN = 200000;
    SpscRingBuffer<int> rb(1024);

    std::thread producer([&] {
        int next = 0;
        while (next < kN) {
            const int n = static_cast<int>(rb.push(&next, 1));
            if (n == 1) {
                ++next;
            } else {
                std::this_thread::yield();
            }
        }
    });

    std::vector<int> received;
    received.reserve(kN);
    int got = 0;
    while (static_cast<int>(received.size()) < kN) {
        if (rb.pop(&got, 1) == 1) {
            received.push_back(got);
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();

    // The SPSC buffer must deliver every item exactly once, in order.
    REQUIRE(received.size() == static_cast<std::size_t>(kN));
    bool ordered = true;
    for (int i = 0; i < kN; ++i) {
        if (received[i] != i) {
            ordered = false;
            break;
        }
    }
    CHECK(ordered);
}
