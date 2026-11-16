#include "starlet/gx_fifo/frame_trace.hpp"

#include "starlet/os/guest_cpu.hpp"
#include "starlet/os/host_allocation.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>

namespace starlet::gx_fifo {
namespace {

struct Event {
    std::uint64_t microseconds;
    std::uint32_t value;
    char kind;
};

constexpr std::size_t capacity = std::size_t{1} << 18;

struct Trace {
    std::unique_ptr<Event[]> events{new Event[capacity]};
    std::atomic<std::size_t> next{0};
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

std::atomic<Trace*> trace{nullptr};

}

void start_frame_trace() {
    if (trace.load(std::memory_order_acquire)) return;
    os::host_allocation_scope host;
    trace.store(new Trace, std::memory_order_release);
    os::set_switch_trace(&frame_event);
}

void frame_event(char kind, std::uint32_t value) {
    Trace* t = trace.load(std::memory_order_acquire);
    if (!t) return;
    const std::size_t i = t->next.fetch_add(1, std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now() - t->start;
    t->events[i & (capacity - 1)] = {std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now).count()), value, kind};
}

bool write_frame_trace(const char* path) {
    Trace* t = trace.load(std::memory_order_acquire);
    if (!t) return false;
    std::FILE* out = std::fopen(path, "w");
    if (!out) return false;
    const std::size_t end = t->next.load(), begin = end > capacity ? end - capacity : 0, count = end - begin;
    for (std::size_t i = begin; i < end; ++i) {
        const Event& e = t->events[i & (capacity - 1)];
        std::fprintf(out, "%llu %c %u\n", (unsigned long long)e.microseconds, e.kind, e.value);
    }
    std::fclose(out);
    std::printf("Frame trace: %zu events in %s\n", count, path);
    return true;
}

}
