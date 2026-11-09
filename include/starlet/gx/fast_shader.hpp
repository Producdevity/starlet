#pragma once

#include "starlet/gx/indirect.hpp"

#include <string>

namespace starlet::gx {

// Visual-fidelity fragment model: TEV in mediump float on normalized values,
// hardware filtering and LOD, and GL blending in place of shader pixel arithmetic.
// GX 8-bit rounding and dithering are not reproduced; comparisons and alpha tests
// quantize to 8 bits. Operands and the output take their low 8 bits as GX does
// where a value can leave 0..255: stage results without clamping, and registers
// marked in wide_registers (bit 2i: colour of register i, bit 2i+1: its alpha).
// Consumes the transform stage outputs.
// Uniforms: vec4 uGxTevRegisters[4], uGxTevKonst[4] (normalized), ivec2
// uGxTevAlphaRef, vec2 uGxCoordinateScale[16], float uGxSamplerBias[8], and the
// indirect uniforms of the exact model. batched programs read the TEV registers
// from the flat gx_register0..3 inputs of a batched transform instead. Coordinates
// in affine are vec2 inputs used without the projective division. fog (type |
// orthographic << 3 | range << 4, see GlesRasterPlan::Fog) adds uGxFog (a, b,
// shift, c), uGxFogColor and, with range adjustment, uGxFogCenter/uGxFogTable[10].
std::string fast_fragment_source(const TevPipeline&, const IndirectSnapshot&, std::uint8_t maps,
                                 bool early_depth, bool constant_alpha, bool batched = false, std::uint8_t affine = 0,
                                 std::uint8_t fog = 0, std::uint8_t wide_registers = 0);

} // namespace starlet::gx
