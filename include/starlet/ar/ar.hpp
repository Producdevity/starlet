#pragma once

#include <cstddef>
#include <cstdint>

namespace starlet::ar {

// ARAM contents, for a DSP model (the DSP reads samples from ARAM directly).
std::uint8_t* memory();
std::size_t bytes();

}
