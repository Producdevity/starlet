#include "audio_ring.hpp"
#include "starlet/host/audio_output.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace starlet::host;
using Clock = std::chrono::steady_clock;

namespace {

void require(bool value, const std::string& why) {
    if (!value) {
        std::fprintf(stderr, "audio: %s\n", why.c_str());
        std::exit(1);
    }
}

// Frame n holds n and its complement, so a lost, repeated or swapped frame shows.
void fill(std::vector<std::int16_t>& frames, std::size_t count, std::uint32_t first) {
    frames.resize(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        frames[i * 2] = std::int16_t(first + i);
        frames[i * 2 + 1] = std::int16_t(~(first + i));
    }
}
bool follows(const std::int16_t* frames, std::size_t count, std::uint32_t first) {
    for (std::size_t i = 0; i < count; ++i)
        if (frames[i * 2] != std::int16_t(first + i) || frames[i * 2 + 1] != std::int16_t(~(first + i))) return false;
    return true;
}

void ring_bounds() {
    AudioRing ring;
    ring.reset(100);
    std::vector<std::int16_t> in, out(400);
    fill(in, 60, 0);
    require(ring.write(in.data(), 60) && ring.level() == 60, "a write that fits");
    require(!ring.write(in.data(), 41) && ring.level() == 60, "a write past the capacity is refused whole");
    fill(in, 40, 60);
    require(ring.write(in.data(), 40) && ring.level() == 100, "the ring fills to its capacity, not its storage");
    require(ring.read(out.data(), 30) == 30 && follows(out.data(), 30, 0), "a partial read");
    require(ring.read(out.data(), 400) == 70 && follows(out.data(), 70, 30), "a read takes what is there");
    require(ring.read(out.data(), 10) == 0 && ring.level() == 0, "an empty read");
    require(ring.write(nullptr, 0) && ring.level() == 0, "an empty write");
}

// Odd sizes against a 128-frame store, starting just before the counters wrap.
void ring_wrap() {
    AudioRing ring;
    ring.reset(100, std::numeric_limits<std::size_t>::max() - 250);
    std::vector<std::int16_t> in, out(200);
    std::uint32_t written = 0, read = 0;
    for (unsigned round = 0; round < 400; ++round) {
        const std::size_t size = 1 + round * 37 % 61;
        fill(in, size, written);
        if (ring.write(in.data(), size)) written += std::uint32_t(size);
        const std::size_t got = ring.read(out.data(), 1 + round * 53 % 47);
        require(follows(out.data(), got, read), "frames keep their order and values across the store's end and the counters' wrap");
        read += std::uint32_t(got);
        require(ring.level() == written - read && ring.level() <= 100, "the level after the wrap");
    }
    require(read > 8000, "the test crossed the wrap");
}

// A producer and a consumer at different paces, without losing or repeating a frame. The
// consumer starts once the producer has found the ring full.
void ring_threads() {
    AudioRing ring;
    ring.reset(1500);
    constexpr std::uint32_t total = 3000000;
    std::atomic<bool> failed{false}, refused{false};
    std::thread consumer([&] {
        while (!refused) std::this_thread::yield();
        std::mt19937 random(7);
        std::vector<std::int16_t> out(2048 * 2);
        std::uint32_t read = 0;
        while (read < total) {
            const std::size_t got = ring.read(out.data(), 1 + random() % 2048);
            if (!follows(out.data(), got, read)) failed = true;
            read += std::uint32_t(got);
            if (random() % 64 == 0) std::this_thread::yield();
        }
    });
    std::mt19937 random(11);
    std::vector<std::int16_t> in;
    std::uint32_t written = 0;
    while (written < total) {
        const std::size_t size = std::min<std::size_t>(1 + random() % 700, total - written);
        fill(in, size, written);
        if (ring.write(in.data(), size)) written += std::uint32_t(size);
        else refused = true;
    }
    consumer.join();
    require(!failed, "the consumer reads every frame once, in order");
}

void wait_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// The producer keeps the ring full, then stops: drops while it is full, underruns once it
// is empty, and the device's rate in between.
void push(bool with_device) {
    AudioOutput output;
    constexpr unsigned rate = 48000;
    output.open_push(rate, 100);
    const AudioStats opened = output.stats();
    require(output.is_open() && output.sample_rate() == rate && opened.sample_rate == rate, "push opens at the rate");
    require(opened.device == with_device && output.device_error().empty() == with_device,
            with_device ? "the dummy driver opens: " + output.device_error() : "no device, and why");
    require(!with_device || opened.device_frames > 0, "the device buffer SDL granted");
    require(opened.ring_frames >= rate / 10, "the ring holds the 100 ms asked for");

    std::vector<std::int16_t> block;
    std::uint32_t written = 0;
    std::uint64_t kept = 0, refused = 0, refused_frames = 0;
    unsigned peak = 0;
    std::uint64_t nonzero = 0;
    const auto started = Clock::now();
    while (Clock::now() - started < std::chrono::milliseconds(800)) {
        fill(block, 240, written);
        if (output.write(block.data(), 240)) {
            for (std::int16_t sample : block) {
                peak = std::max(peak, unsigned(std::abs(int(sample))));
                nonzero += sample != 0;
            }
            kept += 240;
            written += 240;
        } else {
            ++refused;
            refused_frames += 240;
            wait_ms(2);
        }
    }
    const AudioStats full = output.stats();
    require(full.frames == kept && full.dropped_writes == refused && full.dropped_frames == refused_frames,
            "kept and dropped writes are counted");
    require(full.peak == peak && full.nonzero_samples == nonzero, "peak and audible samples of the kept frames");
    require(full.underruns == 0, "no underrun while the producer keeps up: " + std::to_string(full.underruns));
    // 0.8 s at the rate plus the ring that was full at the end.
    const double played = double(kept - full.ring_frames) / rate;
    require(played > 0.5 && played < 1.1, "the device consumes at its rate: " + std::to_string(played) + " s in 0.8 s");

    wait_ms(300);
    const AudioStats dry = output.stats();
    require(dry.underruns > 0 && dry.underrun_frames > 0, "an empty ring underruns");
    require(output.write(block.data(), 240), "writes resume after an underrun");
    output.close();
    require(!output.is_open() && !output.write(block.data(), 240), "no write after close");
}

struct PullCounter {
    std::atomic<std::uint64_t> frames{0}, pulls{0};
    std::atomic<bool> inside{false}, overlapped{false};
    std::atomic<std::size_t> size{0};
};

void pull_slices(void* context, std::int16_t* frames, std::size_t count) noexcept {
    auto& counter = *static_cast<PullCounter*>(context);
    if (counter.inside.exchange(true)) counter.overlapped = true;
    for (std::size_t i = 0; i < count * 2; ++i) frames[i] = std::int16_t(i % 7 ? 1000 : 0);
    counter.size = count;
    counter.frames += count;
    ++counter.pulls;
    counter.inside = false;
}

// The feeder pulls 5 ms slices at the playback rate, serially, and none after close.
void pull(bool with_device) {
    PullCounter counter;
    AudioOutput output;
    constexpr unsigned rate = 32028;
    output.open_pull(rate, pull_slices, &counter);
    require(output.stats().device == with_device, "pull opens with the expected device");
    wait_ms(1000);
    output.close();
    const AudioStats stats = output.stats();
    const std::uint64_t after_close = counter.pulls;
    wait_ms(50);
    require(counter.pulls == after_close, "no pull after close returns");
    require(!counter.overlapped, "pulls are serial");
    constexpr std::size_t slice = rate / 200;
    require(counter.size == slice, "5 ms slices");
    require(stats.frames == counter.frames, "pulled frames are counted");
    // One second at the rate plus the queued start: the device buffer and four slices.
    const double seconds = double(counter.frames - stats.device_frames - 4 * slice) / rate;
    require(seconds > 0.75 && seconds < 1.25, "pulls follow the playback rate: " + std::to_string(seconds) + " s in 1 s");
    constexpr std::uint64_t silent_per_slice = (slice * 2 + 6) / 7;
    require(stats.peak == 1000 && stats.nonzero_samples == counter.pulls * (slice * 2 - silent_per_slice),
            "peak and audible samples of the pulled frames");
    require(stats.underruns == 0, "no underrun while pulls keep up: " + std::to_string(stats.underruns));
    require(stats.dropped_writes == 0, "the ring never refuses the feeder");
}

void rejects() {
    AudioOutput output;
    bool threw = false;
    try {
        output.open_push(100);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw && !output.is_open(), "a rate below 8 kHz is refused");
    threw = false;
    try {
        output.open_pull(48000, nullptr, nullptr);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw && !output.is_open(), "a pull without a function is refused");
    require(!output.write(nullptr, 0), "no write before open");
}

}

int main() {
    ring_bounds();
    ring_wrap();
    ring_threads();
    rejects();
    push(true);
    pull(true);
    // An audio driver SDL lacks stands in for a handheld without a device.
    setenv("SDL_AUDIODRIVER", "starlet-missing", 1);
    push(false);
    pull(false);
    std::puts("audio: ring order and bounds, push and pull pacing with and without a device");
    return 0;
}
