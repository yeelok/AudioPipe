#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace audiopipe {

// Lock-free single-producer / single-consumer ring buffer (spec §6.2). The
// consumer side runs inside the realtime audio callback, so push/pop are
// wait-free and allocation-free after construction. One slot is reserved to
// distinguish the full and empty states, so usable capacity is `capacity`.
template <typename T>
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(std::size_t capacity)
        : buf_(capacity + 1), head_(0), tail_(0) {}

    // Write up to `count` elements; returns how many were written (< count if
    // the buffer filled). Producer thread only.
    std::size_t push(const T* data, std::size_t count) {
        const std::size_t cap  = buf_.size();
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);

        const std::size_t free_space = (tail + cap - head - 1) % cap;
        const std::size_t n          = std::min(count, free_space);
        for (std::size_t i = 0; i < n; ++i) {
            buf_[(head + i) % cap] = data[i];
        }
        head_.store((head + n) % cap, std::memory_order_release);
        return n;
    }

    // Read up to `count` elements; returns how many were read (< count if the
    // buffer emptied). Consumer thread only.
    std::size_t pop(T* out, std::size_t count) {
        const std::size_t cap  = buf_.size();
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);

        const std::size_t avail = (head + cap - tail) % cap;
        const std::size_t n     = std::min(count, avail);
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = buf_[(tail + i) % cap];
        }
        tail_.store((tail + n) % cap, std::memory_order_release);
        return n;
    }

    // Approximate number of elements available to read. Safe to call from
    // either side; the value may be stale by the time it is used.
    std::size_t size() const {
        const std::size_t cap  = buf_.size();
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return (head + cap - tail) % cap;
    }

    std::size_t capacity() const { return buf_.size() - 1; }

private:
    std::vector<T>           buf_;
    std::atomic<std::size_t> head_; // producer advances
    std::atomic<std::size_t> tail_; // consumer advances
};

} // namespace audiopipe
