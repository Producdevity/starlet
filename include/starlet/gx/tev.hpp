#pragma once

#include "starlet/gx/state.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace starlet::gx {

enum class TevColorInput : std::uint8_t {
    prev_rgb, prev_alpha, reg0_rgb, reg0_alpha, reg1_rgb, reg1_alpha, reg2_rgb, reg2_alpha,
    texture_rgb, texture_alpha, raster_rgb, raster_alpha, one, half, konst, zero
};
enum class TevAlphaInput : std::uint8_t { prev, reg0, reg1, reg2, texture, raster, konst, zero };
enum class TevOperation : std::uint8_t {
    add = 0, subtract = 1, r8_gt = 8, r8_eq, gr16_gt, gr16_eq, bgr24_gt, bgr24_eq, rgb8_gt, rgb8_eq
};
enum class TevBias : std::uint8_t { zero, add_half, subtract_half };
enum class TevScale : std::uint8_t { one, two, four, half };
enum class TevRegister : std::uint8_t { previous, reg0, reg1, reg2 };
enum class TevCompare : std::uint8_t { never, less, equal, less_equal, greater, not_equal, greater_equal, always };
enum class TevAlphaLogic : std::uint8_t { and_, or_, xor_, xnor };
enum class TevRaster : std::uint8_t { color0 = 0, color1 = 1, zero = 7 };
enum class TevKonstKind : std::uint8_t { fraction, rgb, component };

struct TevKonstSelection {
    TevKonstKind kind = TevKonstKind::fraction;
    std::uint8_t index = 0; // fraction: (8-index)/8, otherwise K register index.
    std::uint8_t component = 0;
};

template<class Input> struct TevEquation {
    std::array<Input, 4> inputs{};
    TevOperation operation = TevOperation::add;
    TevBias bias = TevBias::zero;
    TevScale scale = TevScale::one;
    bool clamp = false;
    TevRegister destination = TevRegister::previous;
};

struct TevStage {
    TevEquation<TevColorInput> color;
    TevEquation<TevAlphaInput> alpha;
    TevKonstSelection color_konst;
    TevKonstSelection alpha_konst;
    std::array<std::uint8_t, 4> texture_swap{{0, 1, 2, 3}};
    std::array<std::uint8_t, 4> raster_swap{{0, 1, 2, 3}};
    TevRaster raster = TevRaster::zero;
    std::uint8_t texture_map = 0;
    std::uint8_t texture_coord = 0;
    bool texture_enabled = false;
};

struct TevInputUse { bool texture = false; std::uint8_t raster_mask = 0; };
// For a validated stage; alpha comparison A/B operands using the color compare
// result do not consume their encoded alpha inputs.
TevInputUse tev_input_use(const TevStage&);

struct TevPipeline {
    std::array<TevStage, 16> stages{};
    std::uint8_t stage_count = 1;
    std::array<TevCompare, 2> alpha_compare{{TevCompare::always, TevCompare::always}};
    TevAlphaLogic alpha_logic = TevAlphaLogic::and_;
    // BP 0xF5 while its op is active (ZTexture): op 1 add, 2 replace; type 0 Z8, 1 Z16, 2 Z24X8.
    // The last stage's lookup is the Z texel and is unavailable as a TEV input (GX manual 9.12).
    std::uint8_t z_texture_op = 0, z_texture_type = 0;
};
// Whether stage i's texture lookup is consumed: by its TEV inputs, or as the Z texel.
bool tev_stage_samples(const TevPipeline&, unsigned stage);

using TevColor = std::array<std::uint8_t, 4>;
using TevSignedColor = std::array<std::int16_t, 4>;
struct TevUniforms {
    std::array<TevSignedColor, 4> registers{};
    std::array<TevColor, 4> konst{};
    std::array<std::uint8_t, 2> alpha_reference{};
    std::uint32_t z_texture_bias = 0; // BP 0xF4 while a Z texture is active, else 0.
};

struct TevMaterial {
    const TevPipeline pipeline;
    const TevUniforms uniforms;
    const std::uint64_t state_revision;
};

enum class TevError { none, unsupported_state, invalid_pipeline, invalid_uniform, missing_input, unsupported_arithmetic };
struct TevStatus {
    TevError error = TevError::none;
    std::string message;
    explicit operator bool() const { return error == TevError::none; }
};
struct TevCapture {
    TevStatus status;
    std::shared_ptr<const TevMaterial> material;
    explicit operator bool() const { return static_cast<bool>(status); }
};

// Call synchronously at draw capture. Owns TEV configuration and both register
// banks; texture samples, rasterization, fog, depth and EFB operations are external.
// Coordinate lookup operations belong to capture_indirect; bump paths are rejected here.
// Capture/allocation failures propagate
// as documented status or exceptions; callers bound aggregate retained materials.
TevCapture capture_tev(const State& state);
// capture_tev's register banks, alpha references and Z-texture bias, without pipeline validation.
TevUniforms capture_tev_uniforms(const State& state);

// Explicit provisional model, NOT proof of original-GX precision. The fixed8
// policy (coefficient, quantization, clamp and final conversion) is centralized
// in gx_tev.cpp. CPU/GPU agreement establishes only this model's consistency.
enum class TevArithmeticModel : std::uint8_t { fixed8_candidate_v1 = 1 };

struct TevInputs {
    std::array<TevColor, 16> texture{}; // sampled RGBA8 before the stage's swap.
    std::array<TevColor, 2> raster{};   // interpolated/quantized RGBA8 before swap.
    std::uint16_t texture_valid = 0;
    std::uint8_t raster_valid = 0;
};
struct TevEvaluation {
    TevStatus status;
    std::array<TevSignedColor, 4> registers{};
    TevColor color{};
    bool alpha_pass = false;
    explicit operator bool() const { return static_cast<bool>(status); }
};
TevEvaluation evaluate_tev(const TevPipeline&, const TevUniforms&, const TevInputs&, TevArithmeticModel);

struct TevShader {
    TevStatus status;
    // Stable explicit bytes, with schema/model versions. Uniforms and inactive
    // stages are excluded. Caller combines this with other pipeline keys.
    std::vector<std::uint8_t> key;
    // GLSL functions/uniforms for integration into a larger fragment pipeline.
    // gxTevEvaluate takes RGBA8 ivec4 texture[16], raster[2], returning RGBA8.
    // gxTevAlphaPass applies the captured alpha-test configuration.
    std::string functions;
    // Complete GLES3.1 differential-test shader using uniform texture/raster
    // inputs, discard on failed alpha test and an RGBA8-normalized output.
    std::string fragment;
    explicit operator bool() const { return static_cast<bool>(status); }
};
TevShader generate_tev_shader(const TevPipeline&, TevArithmeticModel, bool include_source = true);

} // namespace starlet::gx
