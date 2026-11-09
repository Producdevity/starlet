#pragma once

#include "starlet/gx/state.hpp"
#include <string_view>

namespace starlet::gx {

inline constexpr std::uint32_t depth_code_max = 0x00ffffff;
struct DepthSnapshot {
    const std::uint32_t z_mode, pixel_control;
    const std::uint64_t state_revision;
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
// Captures ordinary, non-AA linear Z24 in RGB8/RGBA6 mode. No depth generation,
// interpolation, clipping, Z texture, Z freeze or framebuffer ownership here.
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
// Inputs are already canonical Z24 codes. Early Z can write when alpha fails;
// late Z needs both tests. Disabling comparison also disables depth writes.
DepthResult evaluate_depth(const DepthSnapshot&, std::uint32_t incoming,
                           std::uint32_t stored, bool alpha_pass);
inline constexpr unsigned depth_glsl_version = 1;
// Pure helper: uvec3 gx_depth(uint incoming, uint stored, bool alpha,
// uvec2 controls). Result is color-accepted, depth-write, retained/new code.
// Controls are captured z_mode/pixel_control; caller validates both codes.
std::string_view depth_glsl();

} // namespace starlet::gx
