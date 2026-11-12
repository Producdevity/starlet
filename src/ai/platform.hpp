#pragma once

// What the AI model needs from the console and the host. platform_starlet.cpp maps these onto
// starlet::os and starlet::host; host tests link their own definitions.

#include "starlet/host/audio_output.hpp"
#include "starlet/sdk/os.h"

#include <string>

namespace starlet::ai::platform {

starlet::sdk::BOOL disable_interrupts();
void restore_interrupts(starlet::sdk::BOOL level);
// Queues handler to run on the guest CPU as an interrupt.
void raise_interrupt(void (*handler)());
void* hardware_to_native(starlet::sdk::u32 address, starlet::sdk::u32 bytes);
// Before the DMA reads game memory (starlet::os::before_dma_read).
void before_dma_read(const void* data, starlet::sdk::u32 bytes);
[[noreturn]] void panic(const char* message);
// Runs fn with host allocation: Starlet's memory, never a game heap.
void with_host_allocation(void (*fn)(void*), void* context);

// Opens the host output at rate with pull as its source, closing an earlier one.
void open_output(unsigned rate, starlet::host::AudioPull pull, void* context);
starlet::host::AudioStats output_stats();
std::string output_device_error();

}
