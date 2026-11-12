#pragma once

#include "starlet/host/audio_output.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace starlet::ai {

// The audio interface DMA. AIInit opens starlet::host::AudioOutput in pull mode at the selected
// rate; its thread plays the programmed blocks. When a block finishes the DMA latches the block
// programmed after it and raises the AI interrupt on the guest CPU, so the game's audio runs at the
// device's playback rate. Nothing programmed within 12 ms of a block's end: the DMA replays the
// last block, as the hardware does when software is late. Without a device a silent clock pulls.

// The device rate for an AI sample-rate selector: the console's 32028.5 and 48043 Hz, rounded.
unsigned output_rate(std::uint32_t selector);

// AIInitDMA with a host pointer (AIInitDMA takes a guest address). The block is native s16 stereo,
// right channel first, 32-byte aligned and a multiple of 32 bytes; it is copied when programmed.
void init_dma(const void* data, std::uint32_t bytes);

// Development: records what the DMA plays to a 16-bit stereo WAV at the AI rate. Before AIInit.
void set_capture(const char* path);

struct Stats {
    bool running = false;
    unsigned sample_rate = 0;
    std::uint64_t frames = 0;       // frames played while the DMA ran
    std::uint64_t completions = 0;  // blocks finished (AI interrupts raised)
    // Blocks that found nothing newly programmed, those that then replayed the previous block,
    // and the time spent waiting for the game.
    std::uint64_t waits = 0, repeats = 0, wait_us = 0;
    starlet::host::AudioStats output;
};
Stats stats();
// Why no audio device is open, empty while one is.
std::string device_error();

}
