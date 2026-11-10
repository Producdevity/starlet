#pragma once

#include "starlet/gx/state.hpp"
#include <array>
#include <string_view>

namespace starlet::gx {

inline constexpr std::uint32_t depth_code_max = 0x00ffffff;

// GXSetZTexture, BP 0xF4/0xF5. op: 0 off, 1 add, 2 replace. type: 0 Z8, 1 Z16, 2 Z24X8.
struct ZTexture {
    std::uint8_t op = 0, type = 0;
    std::uint32_t bias = 0; // 24 bits.
    bool active() const { return op != 0; }
};
// Decodes BP 0xF4/0xF5 words. An active op with a reserved op, type or bit fails; an
// inactive Z texture decodes to {} whatever its other bits hold.
bool decode_z_texture(std::uint32_t bp_f4, std::uint32_t bp_f5, ZTexture&);
// The Z the depth test receives: the texel's Z plus the bias, plus the reference Z with add,
// wrapped to 24 bits. texel is the raw RGBA8 lookup of the last TEV stage, before its swap
// table: Z8 reads A, Z16 A << 8 | R, Z24X8 R << 16 | G << 8 | B. reference is the rasterized
// Z code at the centre of the pixel's 2x2 quad; replace ignores it.
std::uint32_t z_texture_depth(const ZTexture&, const std::array<std::uint8_t, 4>& texel, std::uint32_t reference);

// A fragment program's Z texture: op and type, and whether its Z becomes the fragment depth.
struct ZTextureProgram {
    std::uint8_t op = 0, type = 0;
    bool depth = false;
};
// GLSL of z_texture_depth, empty for op 0. globals declares uniform highp uint uGxZBias.
// reference goes at the start of main(), before any discard: it declares gxZReference with add.
// depth declares highp uint gxZ from texel, a highp uvec4 expression with 0..255 lanes, and
// with ZTextureProgram::depth writes gl_FragDepth = gxZ / 2^24, the encoding of depth.md.
struct ZTextureGlsl { std::string globals, reference, depth; };
ZTextureGlsl z_texture_glsl(const ZTextureProgram&, const std::string& texel);

struct DepthSnapshot {
    const std::uint32_t z_mode, pixel_control;
    const std::uint64_t state_revision;
    const ZTexture z_texture; // An active Z texture makes the depth test late.
};
enum class DepthError { none, unsupported_state, invalid_input };
struct DepthStatus {
    DepthError error = DepthError::none;
    std::string message;
    explicit operator bool() const { return error == DepthError::none; }
};
struct DepthCapture {
    DepthStatus status;
    std::shared_ptr<const DepthSnapshot> snapshot;
    explicit operator bool() const { return static_cast<bool>(status); }
};
// Captures ordinary, non-AA linear Z24 in RGB8/RGBA6 mode, with its Z texture. No depth
// generation, interpolation, clipping, Z freeze or framebuffer ownership here.
DepthCapture capture_depth(const State&);

struct DepthDecision {
    bool color_accepted = false;
    bool depth_write = false;
    std::uint32_t depth_code = 0;
};
struct DepthResult {
    DepthStatus status;
    DepthDecision decision;
    explicit operator bool() const { return static_cast<bool>(status); }
};
// Inputs are already canonical Z24 codes; with a Z texture, incoming is z_texture_depth's
// result. Early Z can write when alpha fails; late Z needs both tests. Disabling comparison
// also disables depth writes.
DepthResult evaluate_depth(const DepthSnapshot&, std::uint32_t incoming,
                           std::uint32_t stored, bool alpha_pass);
inline constexpr unsigned depth_glsl_version = 1;
// Pure helper: uvec3 gx_depth(uint incoming, uint stored, bool alpha,
// uvec2 controls). Result is color-accepted, depth-write, retained/new code.
// Controls are captured z_mode/pixel_control, the latter with bit 6 (early Z) clear
// under an active Z texture; caller validates both codes.
std::string_view depth_glsl();

} // namespace starlet::gx
