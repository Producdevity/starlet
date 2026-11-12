#pragma once

// What the AX model needs from the console underneath it. platform_starlet.cpp maps these onto
// starlet::os and starlet::ar; the host tests link their own definitions.

#include "starlet/sdk/os.h"

#include <cstddef>
#include <cstdint>

namespace starlet::ax::platform {

starlet::sdk::BOOL disable_interrupts();
void restore_interrupts(starlet::sdk::BOOL level);
starlet::sdk::OSTime time();
[[noreturn]] void panic(const char* message);
// ARAM, which the DSP reads sample data from.
const std::uint8_t* aram();
std::size_t aram_bytes();

}
