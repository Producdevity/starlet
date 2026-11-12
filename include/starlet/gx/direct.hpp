#pragma once

#include "starlet/gx/draw.hpp"
#include "starlet/gx/fast_shader.hpp"
#include "starlet/gx/vertex_stream.hpp"

namespace starlet::gx {

// Direct draw domain: the program and per-draw state of plan_gles_draw, derived
// from registers instead of owned snapshots. Only per-vertex arithmetic proofs,
// the offscreen-discard shortcut and full-replacement format detection are
// omitted; vertex values are submitted to the GPU as captured. Every register
// rejection rule of the capture and planning stages still applies.

// exact: the validated candidate arithmetic of plan_gles_draw. fast: the
// visual-fidelity fragment model of gx_fast_shader with GL blending.
enum class DirectShading : std::uint8_t { exact, fast };

// Registers and layout presence that define the program. Equal words always
// plan to the same program; other registers reach the GPU as uniforms/state.
// latched_normal: components of the vectors the XF holds from an earlier vertex (0, 3 or 9);
// see capture_transform_pipeline.
void direct_program_words(const State&, const VertexLayout&, FramebufferFetch, DirectShading, std::vector<std::uint32_t>&,
                          std::uint8_t latched_normal = 0);
// The program word that regular-bank TEV colour registers decide, which
// State::bp_state_revision does not track: equal revisions and equal register words
// mean equal program words.
std::uint32_t direct_program_register_word(const State&, DirectShading);
// Appended to a program's words for its batched or instanced variant.
inline constexpr std::uint32_t batched_program_word = 0xba7c4ed1u;
inline constexpr std::uint32_t instanced_program_word = 0x1a57a2c1u;
// Fast-shading words only: a State and layout whose words equal words, so the program
// can be planned before a draw needs it. False for any other words. latched_normal
// receives the latched-normal components the words record, for plan_direct_program;
// words that record one are refused without it.
bool direct_program_state(const std::vector<std::uint32_t>& words, State&, VertexLayout&,
                          std::uint8_t* latched_normal = nullptr);

struct DirectProgram {
    TransformPipeline transform;
    TevPipeline tev;
    IndirectSnapshot indirect; // Routing and stage selections; matrices are per draw.
    std::uint8_t maps = 0, mip_maps = 0;
    bool rgba6 = false, needs_fetch = false, early_depth = false, fast = false, constant_alpha = false;
    // Fast program for eye-space positions and per-vertex TEV registers; see generate_transform_glsl.
    bool batched = false;
    // Batched: channels (RGB0, RGB1, A0, A1) whose register material travels in vertex colour
    // RGB or alpha; see transform_vertex_materials.
    std::uint8_t vertex_materials = 0;
    // Fast program whose position and normal matrices come per instance; see generate_transform_glsl.
    bool instanced = false;
    std::uint8_t affine = 0; // Fast coordinates with q == 1.
    std::uint8_t fog = 0; // Fast only: fog type | orthographic << 3 | range adjustment << 4.
    ZTextureProgram z_texture; // Op and type of an active Z texture; depth: the draw depth-tests.
    PixelPlan pixel; // RGBA6 only.
    std::string vertex, fragment;
};
struct DirectProgramResult {
    DrawStatus status;
    std::shared_ptr<const DirectProgram> program;
    explicit operator bool() const { return static_cast<bool>(status); }
};
DirectProgramResult plan_direct_program(const State&, const VertexLayout&, FramebufferFetch, DirectShading = DirectShading::exact,
                                        bool batched = false, std::uint8_t latched_normal = 0, bool instanced = false);

struct DirectDrawState {
    GlesRasterPlan raster;
    TevUniforms tev;
    std::array<std::array<float, 2>, 16> coordinate_scale{}, image_size{};
    std::array<std::array<float, 4>, 4> indirect_scale{};
    std::array<std::array<float, 4>, 8> sampler_lod{};
    std::array<std::array<std::int32_t, 4>, 6> indirect_rows{};
    std::array<std::uint32_t, 4> pixel_controls{};
    std::array<TextureDescriptor, 8> textures{}; // Valid for program.maps.
    bool alpha_write = false;
    std::array<float, 3> constant_alpha{}; // Fast programs with constant_alpha: uGxConstantAlpha.
};
DrawStatus plan_direct_state(const State&, const DirectProgram&, TextureCmprModel, DirectDrawState&,
                             GuardBand = GuardBand::reject, FractionalViewport = FractionalViewport::reject);

} // namespace starlet::gx
