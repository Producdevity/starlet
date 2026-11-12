// The audio interface DMA. AIInitDMA programs the next block, which the DMA latches when the
// current one finishes; the latch raises the AI interrupt, so the game programs the block after it
// from the callback, as on the console.
#include "starlet/ai/ai.hpp"

#include "platform.hpp"
#include "starlet/sdk/ai.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

using namespace starlet::sdk;

namespace starlet::ai {
namespace {

using Clock = std::chrono::steady_clock;

// How long a finished block waits for the game to program the next one before the DMA replays
// the previous block.
constexpr auto late_wait = std::chrono::milliseconds(12);

[[noreturn]] void fail(const char* message, unsigned a = 0, unsigned b = 0) {
    std::fprintf(stderr, "starlet: ai: ");
    std::fprintf(stderr, message, a, b);
    std::fprintf(stderr, "\n");
    std::fflush(nullptr);
    std::abort();
}

// A programmed block, copied when it is programmed. Samples are right channel first.
struct Block {
    std::vector<std::int16_t> samples;
    std::size_t frames = 0;
};

struct Capture {
    char path[1024] = {};
    std::FILE* file = nullptr;
    unsigned rate = 0;
    std::uint32_t bytes = 0;

    void open(unsigned sample_rate) {
        if (!path[0] || (file && rate == sample_rate)) return;
        if (file) {
            std::fprintf(stderr, "starlet: ai: capture stops: the rate changed to %u Hz\n", sample_rate);
            std::fclose(file);
            file = nullptr;
            return;
        }
        if (bytes) return;
        file = std::fopen(path, "wb");
        if (!file) {
            std::fprintf(stderr, "starlet: ai: cannot write %s\n", path);
            return;
        }
        rate = sample_rate;
        header();
    }
    void write(const std::int16_t* samples, std::size_t frames) {
        if (!file) return;
        std::fwrite(samples, 4, frames, file);
        bytes += std::uint32_t(frames * 4);
        header();
        std::fseek(file, 0, SEEK_END);
    }
    void header() {
        auto le32 = [](std::uint8_t* p, std::uint32_t v) {
            for (int i = 0; i < 4; ++i) p[i] = std::uint8_t(v >> (8 * i));
        };
        std::uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                              0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
        le32(h + 4, 36 + bytes);
        le32(h + 24, rate);
        le32(h + 28, rate * 4);
        le32(h + 40, bytes);
        std::fseek(file, 0, SEEK_SET);
        std::fwrite(h, 1, sizeof h, file);
        std::fflush(file);
    }
};

struct State {
    std::mutex mutex;
    std::condition_variable programmed;
    bool initialized = false;
    bool output_open = false;
    bool running = false;
    std::uint32_t selector = 0;
    unsigned rate = 0;
    // The DMA registers, and the blocks they and the running transfer use.
    std::uint32_t address = 0, length = 0;
    Block blocks[3];
    int active = -1, pending = -1;
    bool fresh = false;  // programmed since the last latch
    std::size_t offset = 0;
    Stats stats{};
    Capture capture;
};
State state;

// Guest state: the game's DMA callback runs as the AI interrupt.
AIDCallback dma_callback = nullptr;
u32 stream_state = AI_STREAM_STOP;
u8 stream_volume_left = 0, stream_volume_right = 0;
std::atomic<bool> warned_stream{false};

void dma_interrupt() {
    if (dma_callback) dma_callback();
}

void pull(void*, std::int16_t* out, std::size_t frames) noexcept {
    auto& s = state;
    std::size_t written = 0;
    std::unique_lock<std::mutex> lock(s.mutex);
    while (written < frames && s.running) {
        const Block& block = s.blocks[s.active];
        const std::size_t count = std::min(frames - written, block.frames - s.offset);
        const std::int16_t* source = block.samples.data() + s.offset * 2;
        for (std::size_t i = 0; i < count; ++i) {
            out[(written + i) * 2] = source[i * 2 + 1];
            out[(written + i) * 2 + 1] = source[i * 2];
        }
        s.offset += count;
        written += count;
        s.stats.frames += count;
        if (s.offset < block.frames) continue;
        if (!s.fresh) {
            const auto start = Clock::now();
            ++s.stats.waits;
            s.programmed.wait_for(lock, late_wait, [&] { return s.fresh || !s.running; });
            s.stats.wait_us += std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
            if (!s.running) break;
            if (!s.fresh) ++s.stats.repeats;
        }
        s.active = s.pending;
        s.offset = 0;
        s.fresh = false;
        ++s.stats.completions;
        lock.unlock();
        platform::raise_interrupt(dma_interrupt);
        lock.lock();
    }
    std::fill(out + written * 2, out + frames * 2, std::int16_t{0});
    s.capture.write(out, frames);
}

// Opens the device at the selector's rate. Called with the DMA stopped.
void open_output(State& s) {
    s.rate = output_rate(s.selector);
    s.capture.open(s.rate);
    platform::open_output(s.rate, pull, nullptr);
    s.output_open = true;
    const std::string error = platform::output_device_error();
    if (!error.empty()) std::fprintf(stderr, "starlet: ai: %s; a silent clock paces the DMA\n", error.c_str());
}

void resize_block(void* context) {
    auto* request = static_cast<std::pair<Block*, std::uint32_t>*>(context);
    request->first->samples.resize(request->second / 2);
}

// register_address is what AIGetDMAStartAddr returns: the guest's address, or the host pointer's low bits.
void program(const void* data, std::uint32_t bytes, std::uint32_t register_address) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    if (!data || (address & 31) || !bytes || (bytes & 31) || bytes > 0x7FFFu * 32)
        fail("invalid DMA block 0x%x (%u bytes)", register_address, bytes);
    auto& s = state;
    int free = 0;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        while (free == s.active || free == s.pending) ++free;
        s.address = register_address;
        s.length = bytes;
    }
    // Only the DMA's active and pending blocks are shared with the output thread.
    Block& block = s.blocks[free];
    if (block.samples.size() != bytes / 2) {
        std::pair<Block*, std::uint32_t> request{&block, bytes};
        platform::with_host_allocation(resize_block, &request);
    }
    platform::before_dma_read(data, bytes);
    std::memcpy(block.samples.data(), data, bytes);
    block.frames = bytes / 4;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.pending = free;
        s.fresh = true;
    }
    s.programmed.notify_all();
}

}

unsigned output_rate(std::uint32_t selector) { return selector ? 48043 : 32028; }

void init_dma(const void* data, std::uint32_t bytes) {
    program(data, bytes, std::uint32_t(reinterpret_cast<std::uintptr_t>(data)));
}

void set_capture(const char* path) {
    if (state.initialized) fail("the capture is set after AIInit");
    std::snprintf(state.capture.path, sizeof state.capture.path, "%s", path ? path : "");
}

Stats stats() {
    Stats out;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        out = state.stats;
        out.running = state.running;
        out.sample_rate = state.rate;
    }
    out.output = platform::output_stats();
    return out;
}

std::string device_error() { return platform::output_device_error(); }

}

using namespace starlet::ai;

extern "C" {

void AIInit(u8*) {
    auto& s = state;
    if (s.initialized) return;
    dma_callback = nullptr;
    s.selector = AI_SAMPLERATE_32KHZ;
    if (!s.output_open || s.rate != output_rate(s.selector)) open_output(s);
    s.initialized = true;
}

BOOL AICheckInit() { return state.initialized; }

// As the SDK: AIInit initializes again afterwards; the DMA and the output keep running.
void AIReset() { state.initialized = false; }

AIDCallback AIRegisterDMACallback(AIDCallback callback) {
    const BOOL enabled = platform::disable_interrupts();
    const AIDCallback previous = dma_callback;
    dma_callback = callback;
    platform::restore_interrupts(enabled);
    return previous;
}

void AIInitDMA(u32 start_addr, u32 length) {
    void* samples = platform::hardware_to_native(start_addr, length);
    if (!samples) {
        char message[128];
        std::snprintf(message, sizeof message, "AI DMA from 0x%08x (0x%x bytes) outside the game's memory", start_addr, length);
        platform::panic(message);
    }
    program(samples, length, start_addr);
}

void AIStartDMA() {
    auto& s = state;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.initialized || s.pending < 0) fail("AIStartDMA before AIInit and AIInitDMA");
        if (s.running) return;
        s.running = true;
        s.active = s.pending;
        s.offset = 0;
        s.fresh = false;
    }
    s.programmed.notify_all();
}

void AIStopDMA() {
    auto& s = state;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.running = false;
    }
    s.programmed.notify_all();
}

BOOL AIGetDMAEnableFlag() {
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.running;
}

u32 AIGetDMAStartAddr() {
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.address;
}

u32 AIGetDMALength() {
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.length;
}

u32 AIGetDMABytesLeft() {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.running) return 0;
    return u32((state.blocks[state.active].frames - state.offset) * 4);
}

u32 AIGetDSPSampleRate() { return state.selector; }

void AISetDSPSampleRate(u32 rate) {
    auto& s = state;
    const u32 selector = rate ? AI_SAMPLERATE_48KHZ : AI_SAMPLERATE_32KHZ;
    if (selector == s.selector) return;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.running) fail("AISetDSPSampleRate(%u) while the DMA runs", rate);
    }
    s.selector = selector;
    if (s.initialized) open_output(s);
}

// Disc audio streaming has no source here: the registers keep their values and the stream is silent.
void AISetStreamPlayState(u32 play) {
    if (play == AI_STREAM_START && !warned_stream.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "starlet: ai: disc audio streaming is not modelled; the stream stays silent\n");
    stream_state = play;
}

u32 AIGetStreamPlayState() { return stream_state; }
void AISetStreamVolLeft(u8 vol) { stream_volume_left = vol; }
void AISetStreamVolRight(u8 vol) { stream_volume_right = vol; }
u8 AIGetStreamVolLeft() { return stream_volume_left; }
u8 AIGetStreamVolRight() { return stream_volume_right; }

}
