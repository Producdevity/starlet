#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace starlet::host {

// Stereo s16 frames from one producer thread to one consumer thread, without locks. The
// producer advances head and the consumer tail; each counts frames since reset, so their
// difference is the fill level after the counters wrap too. Storage is a power of two, so
// a frame's slot stays the same across that wrap.
class AudioRing {
public:
    // Holds at most capacity_frames. Tests start the counters at first to cross their wrap.
    void reset(std::size_t capacity_frames, std::size_t first = 0) {
        std::size_t storage = 1;
        while (storage < capacity_frames) storage *= 2;
        samples_.assign(storage * 2, 0);
        mask_ = storage - 1;
        capacity_ = capacity_frames;
        head_.store(first, std::memory_order_relaxed);
        tail_.store(first, std::memory_order_relaxed);
    }
    std::size_t capacity() const noexcept { return capacity_; }
    // Frames written and not yet read. Either side may ask.
    std::size_t level() const noexcept {
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return head_.load(std::memory_order_acquire) - tail;
    }
    // Producer: writes every frame, or none when they do not fit.
    bool write(const std::int16_t* frames, std::size_t count) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (count > capacity_ - (head - tail_.load(std::memory_order_acquire))) return false;
        if (!count) return true;
        const std::size_t at = head & mask_, first = std::min(count, mask_ + 1 - at);
        std::memcpy(samples_.data() + at * 2, frames, first * 4);
        std::memcpy(samples_.data(), frames + first * 2, (count - first) * 4);
        head_.store(head + count, std::memory_order_release);
        return true;
    }
    // Consumer: reads up to count frames and returns how many it read.
    std::size_t read(std::int16_t* frames, std::size_t count) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t taken = std::min(count, head_.load(std::memory_order_acquire) - tail);
        if (!taken) return 0;
        const std::size_t at = tail & mask_, first = std::min(taken, mask_ + 1 - at);
        std::memcpy(frames, samples_.data() + at * 2, first * 4);
        std::memcpy(frames + first * 2, samples_.data(), (taken - first) * 4);
        tail_.store(tail + taken, std::memory_order_release);
        return taken;
    }

private:
    std::vector<std::int16_t> samples_;
    std::size_t mask_ = 0, capacity_ = 0;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

}
