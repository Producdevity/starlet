#pragma once

#include "starlet/gx/tev.hpp"

namespace starlet::gx {

struct IndirectLookup {
    std::uint8_t map = 0, coordinate = 0, shift_s = 0, shift_t = 0;
};
struct IndirectStage {
    std::uint8_t lookup = 0, matrix = 0, bias = 0; // matrix 0: no offset; 1..3: static matrix.
    // Wrap of the regular coordinate before the offset: 0 none, 1..5 modulo 256..16 texels, 6 to zero.
    std::uint8_t wrap_s = 0, wrap_t = 0;
    // Offset format of a stage with a matrix: 0 8-bit, 1..3 the low 5, 4 or 3 bits (bias +1).
    std::uint8_t format = 0;
    // 1: texture LOD from the coordinate before wrapping and offset.
    std::uint8_t lod_unmodified = 0;
};
struct IndirectMatrix {
    std::array<std::array<std::int32_t, 4>, 2> rows{}; // signed11 coefficients, signed exponent.
};
struct IndirectSnapshot {
    std::array<IndirectLookup, 4> lookups{};
    std::array<IndirectStage, 16> stages{};
    std::array<IndirectMatrix, 3> matrices{};
    std::uint8_t lookup_mask = 0;
    std::uint64_t state_revision = 0;
};
struct IndirectCapture {
    TevStatus status;
    std::shared_ptr<const IndirectSnapshot> snapshot;
    explicit operator bool() const { return static_cast<bool>(status); }
};
// Static matrices and 8-, 5-, 4- or 3-bit offsets with optional wrapping and LOD from the unmodified
// coordinate; no feedback, bump alpha or dynamic matrices.
// Stages that only wrap (matrix off, or a disabled lookup) keep matrix 0 with their wrap modes.
IndirectCapture capture_indirect(const State&, const TevPipeline&);

// Candidate S17.7 offset arithmetic; GPU agreement is not original-GX precision proof.
std::array<std::int32_t, 2> evaluate_indirect_offset(const IndirectMatrix&, TevColor, std::uint8_t bias,
                                                   std::uint8_t format = 0);
// GLSL for gxIndirectOffset's sample argument (masked to the stage's format) and its bias (subtracted).
std::string indirect_sample_glsl(const IndirectStage&, const std::string& sample);
std::string indirect_bias_glsl(const IndirectStage&);
const char* indirect_offset_glsl();
// True when a stage's coordinate goes through the S17.7 indirect path.
inline bool indirect_coordinate(const IndirectStage& s) { return s.matrix || s.wrap_s || s.wrap_t; }
inline bool indirect_coordinates(const IndirectSnapshot& snapshot) {
    for (const auto& s : snapshot.stages) if (indirect_coordinate(s)) return true;
    return false;
}
// GLSL for a stage's regular S17.7 coordinate `base` (an ivec2) after its wrap modes.
std::string indirect_wrap_glsl(const IndirectStage&, const std::string& base);

} // namespace starlet::gx
