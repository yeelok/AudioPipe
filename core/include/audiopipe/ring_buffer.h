#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace audiopipe {

// Lock-free single-producer / single-consumer ring buffer (spec §6.2). The
// consumer side runs inside the realtime audio callback, so push/pop must be
// wait-free and allocation-free once constructed.
//
// TODO(M1): implement push/pop with acquire/release ordering + tests
// (fill/drain, wrap-around, no tearing).
template <typename T>
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(std::size_t capacity)
        : buf_(capacity + 1), head_(0), tail_(0) {}

    // Write up to `count` elements; returns how many were written (< count if
    // the buffer filled).
    std::size_t push(const T* data, std::size_t count);

    // Read up to `count` elements; returns how many were read (< count if the
    // buffer emptied).
    std::size_t pop(T* out, std::size_t count);

private:
    std::vector<T>           buf_;
    std::atomic<std::size_t> head_; // producer advances
    std::atomic<std::size_t> tail_; // consumer advances
};

} // namespace audiopipe
