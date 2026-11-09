#pragma once

#include "starlet/gx/state.hpp"
#include <string_view>

namespace starlet::gx {

enum class PixelPolicy { documented_only, candidate_v1 };
enum class PixelFormat { rgb8, rgba6 };
enum class PixelOperation { copy, blend, subtract, logic };
enum PixelAssumption : std::uint32_t {
    product_ties_up = 1u << 0,
    destination_msb_expansion = 1u << 1,
    alpha_truncation = 1u << 2,
    nondither_rgb_truncation = 1u << 3,
    subtract_saturation = 1u << 4
};
struct PixelPlan {
    PixelFormat format = PixelFormat::rgb8;
    PixelOperation operation = PixelOperation::copy;
    std::uint8_t source_factor = 0, destination_factor = 0, logic_operation = 0, forced_alpha = 0;
    bool color_write = false, alpha_write = false, dither = false, force_alpha = false;
};
enum class PixelError { none, unsupported_state, unproven_arithmetic, invalid_input };
struct PixelStatus {
    PixelError error = PixelError::none;
    std::string message;
    explicit operator bool() const { return error == PixelError::none; }
};
struct PixelCapture;
class PixelSnapshot {
public:
    const PixelPlan& plan() const { return plan_; }
    const EfbLayout& efb() const { return efb_; }
    PixelPolicy policy() const { return policy_; }
    std::uint32_t assumptions() const { return assumptions_; }
    std::uint64_t state_revision() const { return revision_; }
    const std::array<std::uint32_t,4>& words() const { return words_; } // BP00,41,42,43.
    // gx_pixel controls: BP41, BP42, format(0/1), arithmetic-version(1).
    std::array<std::uint32_t,4> uniforms() const { return {{words_[1],words_[2],words_[3]&7u,1}}; }
private:
    friend PixelCapture capture_pixel(const State&, PixelPolicy);
    PixelSnapshot() = default;
    PixelPlan plan_;
    EfbLayout efb_;
    PixelPolicy policy_ = PixelPolicy::documented_only;
    std::uint32_t assumptions_ = 0;
    std::uint64_t revision_ = 0;
    std::array<std::uint32_t,4> words_{};
};
struct PixelCapture {
    PixelStatus status;
    std::shared_ptr<const PixelSnapshot> snapshot;
    explicit operator bool() const { return static_cast<bool>(status); }
};
// Color arithmetic only: this does not admit depth, fog, Z-texture or a draw.
PixelCapture capture_pixel(const State&, PixelPolicy = PixelPolicy::documented_only);

using PixelColor = std::array<std::uint8_t,4>;
struct PixelResult {
    PixelStatus status;
    PixelColor codes{};
    explicit operator bool() const { return static_cast<bool>(status); }
};
// Source RGBA8; destination RGBA6 codes 0..63 or RGB8 bytes with A=255.
// Coordinates are EFB x-right/y-down, within 640x456. A rejected color preserves
// every destination code. Caller computes acceptance; no depth side effect here.
PixelResult evaluate_pixel(const PixelSnapshot&, PixelColor source, PixelColor destination,
                           int efb_x, int efb_y, bool color_accepted = true);

inline constexpr unsigned pixel_glsl_version = 1;
// Pure GLES3.1 integer helper: uvec4 gx_pixel(uvec4 source, uvec4 destination,
// ivec2 efb, bool accepted, uvec4 controls). Same valid-input contract as CPU.
// No uniforms/main/fetch/storage conversion/depth/GL state is declared here.
std::string_view pixel_glsl();
// gx_pixel for one RGBA6 copy/blend/subtract plan with operation, factors, masks,
// dither and the forced-alpha enable fixed at compile time: uvec4
// gxPixelStage(uvec4 source, uvec4 destination_codes, ivec2 efb, uint forced_alpha)
// returns the same codes as gx_pixel(source, destination_codes, efb, true, controls).
std::string pixel_stage_glsl(const PixelPlan&);

} // namespace starlet::gx
