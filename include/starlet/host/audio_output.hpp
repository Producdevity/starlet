#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace starlet::host {

// Fills count interleaved native s16 stereo frames, left then right. Pulls are serial.
using AudioPull = void (*)(void* context, std::int16_t* frames, std::size_t count) noexcept;

struct AudioStats {
    unsigned sample_rate = 0;
    bool device = false;            // false: no device opened; a silent clock consumes the frames
    unsigned device_frames = 0;     // the device buffer SDL granted
    unsigned ring_frames = 0;       // capacity of the ring between the port and the device
    unsigned peak = 0;              // largest sample magnitude taken from the port
    std::uint64_t frames = 0;       // frames taken from the port: pulled, or written and kept
    std::uint64_t nonzero_samples = 0;
    // Device requests the ring could not fill, an audible gap, and the silence they got.
    std::uint64_t underruns = 0, underrun_frames = 0;
    // Push: writes refused whole because the ring was full.
    std::uint64_t dropped_writes = 0, dropped_frames = 0;
};

// The firmware's SDL2 audio on its default route, fed from a bounded single-producer ring.
// It never changes system volume, mute or mixer settings, and plays the port's samples
// unchanged at its rate (SDL converts to the device's format). Without a device it reports
// why and a silent clock consumes the frames at the rate, so a port runs the same.
//
// The device plays once the ring first holds its buffer plus four 5 ms slices; after that
// a request the ring cannot fill gets silence for the rest and counts as an underrun.
// open_pull: a Starlet thread pulls 5 ms slices on a steady clock and keeps that much
// queued, so a port's DMA completions stay evenly spaced even when the device takes 2048
// frames or more at once. open_push: the port writes frames as it produces them.
//
// open_*, close and the destructor allocate and free host memory: a port with its own
// operator new calls them in its host-allocation scope, from one thread at a time.
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    void open_pull(unsigned sample_rate, AudioPull pull, void* context);
    // The ring holds buffer_ms of audio, at least the start level plus four slices; a write
    // that does not fit is dropped whole.
    void open_push(unsigned sample_rate, unsigned buffer_ms = 120);
    // Push mode, from one producer thread at a time. False when the frames were dropped.
    bool write(const std::int16_t* frames, std::size_t count) noexcept;
    // No pull and no device request runs once it returns.
    void close() noexcept;

    bool is_open() const noexcept;
    unsigned sample_rate() const noexcept;
    // Why no device is open, empty while one is.
    std::string device_error() const;
    AudioStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
