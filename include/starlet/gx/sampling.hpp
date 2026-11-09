#pragma once

#include "starlet/gx/texture.hpp"

namespace starlet::gx {
// Floating candidate edge/diagonal isotropic LOD; no anisotropy or bias clamp.
float evaluate_texture_lod(std::array<float,2> dx, std::array<float,2> dy, const TextureSampler&);
const char* texture_sampling_glsl();
} // namespace starlet::gx
