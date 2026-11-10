#include "starlet/host/audio_output.hpp"

#include "audio_ring.hpp"

#include <SDL.h>

#ifdef __linux__
#include <pthread.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace starlet::host {
namespace {

using Clock = std::chrono::steady_clock;

// The device buffer asked of SDL; firmware often grants more.
constexpr Uint16 requested_device_frames = 512;

void name_thread(const char* name) {
#ifdef __linux__
    pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}

enum class Mode { closed, pull, push };

}

struct AudioOutput::Impl {
    std::atomic<Mode> mode{Mode::closed};
    unsigned rate = 0;
    AudioPull pull = nullptr;
    void* context = nullptr;
    AudioRing ring;
    SDL_AudioDeviceID device = 0;
    bool sdl_audio = false;
    std::thread feeder, silent;
    std::atomic<bool> running{false};
    // The device side plays silence until the ring first holds start_level frames, the device
    // buffer plus four slices; underruns count from then.
    std::size_t start_level = 0;
    std::atomic<bool> primed{false};
    std::atomic<bool> device_open{false};
    std::atomic<unsigned> sample_rate{0}, device_frames{0}, ring_frames{0}, peak{0};
    std::atomic<std::uint64_t> frames{0}, nonzero{0}, underruns{0}, underrun_frames{0}, dropped_writes{0},
        dropped_frames{0};
    mutable std::mutex error_mutex;
    std::string error;

    void set_error(std::string text) {
        std::lock_guard<std::mutex> lock(error_mutex);
        error = std::move(text);
    }

    void account(const std::int16_t* samples, std::size_t count) noexcept {
        unsigned high = 0;
        std::uint64_t audible = 0;
        for (std::size_t i = 0; i < count * 2; ++i) {
            const int value = samples[i];
            if (value) ++audible;
            high = std::max(high, unsigned(value < 0 ? -value : value));
        }
        unsigned old = peak.load(std::memory_order_relaxed);
        while (old < high && !peak.compare_exchange_weak(old, high, std::memory_order_relaxed)) {}
        frames.fetch_add(count, std::memory_order_relaxed);
        nonzero.fetch_add(audible, std::memory_order_relaxed);
    }

    // The device's side of the ring: SDL's callback, or the silent clock.
    void consume(std::int16_t* out, std::size_t count) noexcept {
        if (!primed.load(std::memory_order_relaxed)) {
            if (ring.level() < start_level) {
                std::memset(out, 0, count * 4);
                return;
            }
            primed.store(true, std::memory_order_relaxed);
        }
        const std::size_t got = ring.read(out, count);
        if (got == count) return;
        std::memset(out + got * 2, 0, (count - got) * 4);
        underruns.fetch_add(1, std::memory_order_relaxed);
        underrun_frames.fetch_add(count - got, std::memory_order_relaxed);
    }

    static void SDLCALL callback(void* self, Uint8* stream, int bytes) {
        static_cast<Impl*>(self)->consume(reinterpret_cast<std::int16_t*>(stream), std::size_t(bytes) / 4);
    }

    void open_device() {
        set_error({});
        if (!sdl_audio) {
            if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
                set_error(std::string("SDL audio: ") + SDL_GetError());
                return;
            }
            sdl_audio = true;
        }
        SDL_AudioSpec desired{}, obtained{};
        desired.freq = int(rate);
        desired.format = AUDIO_S16SYS;
        desired.channels = 2;
        desired.samples = requested_device_frames;
        desired.callback = &Impl::callback;
        desired.userdata = this;
        // No allowed changes: SDL converts to whatever the device plays. The device opens paused.
        device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
        if (!device) {
            set_error(std::string("SDL audio device: ") + SDL_GetError());
            return;
        }
        if (obtained.freq != int(rate) || obtained.format != AUDIO_S16SYS || obtained.channels != 2 || !obtained.samples) {
            SDL_CloseAudioDevice(device);
            device = 0;
            set_error("SDL audio device: it did not keep the s16 stereo format");
            return;
        }
        device_frames = obtained.samples;
        device_open = true;
    }

    // Pull mode: one ~5 ms slice per tick on a steady clock, with the device buffer plus four
    // slices queued, which also absorbs drift between the device's clock and the host's.
    void feed() noexcept {
        name_thread("starlet-audio");
        SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
        const std::size_t size = rate / 200;
        const std::size_t high = device_frames.load() + 8 * size;
        std::vector<std::int16_t> slice(size * 2);
        const auto produce = [&] {
            std::fill(slice.begin(), slice.end(), std::int16_t{0});
            pull(context, slice.data(), size);
            account(slice.data(), size);
            if (!ring.write(slice.data(), size)) {
                dropped_writes.fetch_add(1, std::memory_order_relaxed);
                dropped_frames.fetch_add(size, std::memory_order_relaxed);
            }
        };
        const auto tick = std::chrono::nanoseconds(std::uint64_t(size) * 1000000000u / rate);
        while (running.load(std::memory_order_acquire) && ring.level() < start_level) produce();
        auto next = Clock::now() + tick;
        while (running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_until(next);
            const auto now = Clock::now();
            next = now - next > 4 * tick ? now + tick : next + tick;
            std::size_t queued = ring.level();
            if (queued > high) continue;
            produce();
            queued += size;
            while (queued < size * 2 && running.load(std::memory_order_acquire)) {
                produce();
                queued += size;
            }
        }
    }

    // Without a device the frames still leave the ring at the playback rate.
    void silent_clock() noexcept {
        name_thread("starlet-silence");
        const std::size_t size = rate / 200;
        std::vector<std::int16_t> buffer(size * 2);
        const auto period = std::chrono::nanoseconds(std::uint64_t(size) * 1000000000u / rate);
        auto next = Clock::now() + period;
        while (running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_until(next);
            consume(buffer.data(), size);
            next += period;
            if (Clock::now() - next > 8 * period) next = Clock::now() + period;
        }
    }

    // ring_capacity: at least the start level plus four slices.
    void start(Mode opened, unsigned sample_rate_hz, std::size_t ring_capacity) {
        const std::size_t slice = rate / 200;
        start_level = device_frames.load() + 4 * slice;
        ring_capacity = std::max(ring_capacity, start_level + 4 * slice);
        ring.reset(ring_capacity);
        ring_frames = unsigned(ring_capacity);
        mode = opened;
        running.store(true, std::memory_order_release);
        try {
            if (opened == Mode::pull) feeder = std::thread([this] { feed(); });
            if (device) SDL_PauseAudioDevice(device, 0);
            else silent = std::thread([this] { silent_clock(); });
        } catch (...) {
            stop();
            throw;
        }
        sample_rate = sample_rate_hz;
    }

    void begin(unsigned sample_rate_hz) {
        stop();
        if (sample_rate_hz < 8000 || sample_rate_hz > 192000)
            throw std::invalid_argument("audio output: sample rate " + std::to_string(sample_rate_hz) + " Hz is out of range");
        rate = sample_rate_hz;
        primed = false;
        peak = 0;
        device_frames = 0;
        for (auto* counter : {&frames, &nonzero, &underruns, &underrun_frames, &dropped_writes, &dropped_frames}) *counter = 0;
        open_device();
    }

    void stop() noexcept {
        running.store(false, std::memory_order_release);
        if (feeder.joinable()) feeder.join();
        if (device) {
            // Waits for a callback in progress.
            SDL_CloseAudioDevice(device);
            device = 0;
        }
        if (silent.joinable()) silent.join();
        if (sdl_audio) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            sdl_audio = false;
        }
        device_open = false;
        mode = Mode::closed;
        pull = nullptr;
        context = nullptr;
        sample_rate = 0;
    }
};

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}

AudioOutput::~AudioOutput() { close(); }

void AudioOutput::open_pull(unsigned sample_rate, AudioPull pull, void* context) {
    if (!pull) throw std::invalid_argument("audio output: no pull function");
    auto& s = *impl_;
    s.begin(sample_rate);
    s.pull = pull;
    s.context = context;
    s.start(Mode::pull, sample_rate, s.device_frames.load() + 16 * (sample_rate / 200));
}

void AudioOutput::open_push(unsigned sample_rate, unsigned buffer_ms) {
    if (!buffer_ms) throw std::invalid_argument("audio output: an empty push buffer");
    auto& s = *impl_;
    s.begin(sample_rate);
    s.start(Mode::push, sample_rate, std::max<std::size_t>(std::size_t(sample_rate) * buffer_ms / 1000, 1));
}

bool AudioOutput::write(const std::int16_t* frames, std::size_t count) noexcept {
    auto& s = *impl_;
    if (s.mode != Mode::push || (!frames && count)) return false;
    if (!s.ring.write(frames, count)) {
        s.dropped_writes.fetch_add(1, std::memory_order_relaxed);
        s.dropped_frames.fetch_add(count, std::memory_order_relaxed);
        return false;
    }
    s.account(frames, count);
    return true;
}

void AudioOutput::close() noexcept { impl_->stop(); }

bool AudioOutput::is_open() const noexcept { return impl_->sample_rate.load() != 0; }

unsigned AudioOutput::sample_rate() const noexcept { return impl_->sample_rate.load(); }

std::string AudioOutput::device_error() const {
    std::lock_guard<std::mutex> lock(impl_->error_mutex);
    return impl_->error;
}

AudioStats AudioOutput::stats() const noexcept {
    const auto& s = *impl_;
    AudioStats out;
    out.sample_rate = s.sample_rate.load();
    out.device = s.device_open.load();
    out.device_frames = s.device_frames.load();
    out.ring_frames = s.ring_frames.load();
    out.peak = s.peak.load();
    out.frames = s.frames.load();
    out.nonzero_samples = s.nonzero.load();
    out.underruns = s.underruns.load();
    out.underrun_frames = s.underrun_frames.load();
    out.dropped_writes = s.dropped_writes.load();
    out.dropped_frames = s.dropped_frames.load();
    return out;
}

}
