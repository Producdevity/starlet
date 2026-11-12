#pragma once

#include <cstdint>

namespace starlet::ax {

// AX plays through starlet::ai as the console's AXOut does: AXInit registers an AI DMA callback,
// and each AI interrupt programs the buffer mixed one interrupt earlier, mixes the next 5 ms frame
// (160 samples at 32 kHz) on the guest CPU and runs the game's frame callback. The port calls
// AIInit before AXInit, as the console's games do; starlet::ai reports late frames.

struct Stats {
    std::uint64_t frames = 0;          // frames mixed
    std::uint64_t dropped_voices = 0;  // voices the SDK's DSP budget dropped (their callbacks ran)
    unsigned voices = 0;               // voices synced in the last frame
    unsigned dsp_cycles = 0;           // the SDK's DSP cycle estimate for the last frame (AXGetDspCycles)
    unsigned max_dsp_cycles = 0;       // AXGetMaxDspCycles
};

Stats stats() noexcept;

}
