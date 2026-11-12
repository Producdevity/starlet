#pragma once

// A guest CPU and an audio device the tests drive by hand.

#include "starlet/host/audio_output.hpp"
#include "starlet/sdk/os.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fake_ai {

// Interrupts run inside the pull that raises them, or wait for run_interrupts.
extern bool immediate;
extern unsigned output_rate;
extern int opens;
// Guest memory: hardware address 0x80000000 is its first byte.
// 32-byte aligned like the console's DMA buffers (a vector's storage is not, on glibc).
struct GuestMemory {
    alignas(32) std::uint8_t bytes[1 << 20];
    std::uint8_t* data() { return bytes; }
    std::size_t size() const { return sizeof bytes; }
    std::uint8_t& operator[](std::size_t i) { return bytes[i]; }
};
extern GuestMemory memory;

void run_interrupts();
std::size_t queued_interrupts();
// Pulls frames from the AI output on the calling thread.
std::vector<std::int16_t> pull(std::size_t frames);

}
