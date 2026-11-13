#pragma once

#include "starlet/gx/depth.hpp"
#include "starlet/gx/raster.hpp"
#include "starlet/gx/pixel.hpp"
#include "starlet/gx/tev.hpp"
#include "starlet/gx/indirect.hpp"
#include "starlet/gx/sampling.hpp"
#include "starlet/gx/texture.hpp"
#include "starlet/gx/transform.hpp"

namespace starlet::gx {

struct DrawLimits {
    VertexLimits vertices;
    TextureLimits textures;
    std::size_t max_retained_bytes = 32 * 1024 * 1024;
};
struct DrawPacket {
    std::shared_ptr<const VertexSnapshot> vertices;
    std::shared_ptr<const TransformSnapshot> transform;
    std::shared_ptr<const TevMaterial> tev;
    std::shared_ptr<const IndirectSnapshot> indirect;
    std::shared_ptr<const RasterSnapshot> raster;
    std::shared_ptr<const PixelSnapshot> pixel;
    std::array<std::shared_ptr<const TextureSnapshot>, 8> textures{};
    std::uint8_t texture_mask = 0;
    std::size_t retained_bytes = 0;
};
enum class DrawError { none, vertices, transform, tev, raster, texture, missing_input, storage_limit, unsupported_state, guard_band };
struct DrawStatus {
    DrawError error = DrawError::none;
    std::string message;
    explicit operator bool() const { return error == DrawError::none; }
};
struct DrawCapture {
    DrawStatus status;
    std::shared_ptr<const DrawPacket> packet;
    explicit operator bool() const { return static_cast<bool>(status); }
};
// Synchronous: complete immutable capture or no packet. Captures each consumed map
// once, even if multiple TEV stages use it. Optional texture/vertex caches verify
// current source bytes; native pointer identity cannot establish unchanged data.
// max_retained_bytes bounds published payload, not allocator or temporary read
// storage; per-component limits additionally bound each capture's scratch.
// The caller bounds retained packets and preserves source bytes until return.
DrawCapture capture_draw(const State&, const Draw&, MemoryReader&, DrawLimits = {},
                         TextureCmprModel = TextureCmprModel::reject_unverified, TextureCache* = nullptr);
DrawCapture capture_draw(const State&, const Draw&, MemoryReader&, DrawLimits,
                         TextureCmprModel, TextureCache*, VertexCache*);

enum class FramebufferFetch { none, arm, ext };
struct GlesDrawPipeline {
    GlesRasterPlan raster;
    std::array<std::array<float, 2>, 16> coordinate_scale{}, image_size{};
    std::array<std::array<float, 4>, 4> indirect_scale{};
    std::array<std::array<float, 4>, 8> sampler_lod{};
    std::vector<std::uint8_t> key;
    std::string vertex, fragment;
    std::array<std::uint32_t,4> pixel_controls{};
    bool rgba6_copy = false, full_replacement = false, alpha_write = false;
    // Every triangle lies outside the scissored viewport; submit no GPU work.
    bool discarded = false;
};
struct GlesDrawResult {
    DrawStatus status;
    GlesDrawPipeline pipeline;
    explicit operator bool() const { return static_cast<bool>(status); }
};
// GPU integration domain: orthographic/perspective triangles, RGB8 fixed-function blending
// or explicit candidate RGBA6 arithmetic with coherent framebuffer fetch for
// blending/subtraction, linear depth comparisons/writes, Z textures and GX early/late Z;
// automatic power-of-two repeat bias only, no cylindrical modifiers,
// matching min/mag spatial filters, isotropic edge/diagonal LOD, mip nearest
// or trilinear filtering, and no anisotropy or bias clamp.
// Uses explicit candidate transform/TEV arithmetic; filtered texels and raster
// colors are rounded to RGBA8. This is not original-hardware precision proof.
// Key-only planning retains all current draw/uniform/vertex validation.
GlesDrawResult plan_gles_draw(const DrawPacket&, FramebufferFetch = FramebufferFetch::none, bool include_source = true);
// The fragment stage both planners emit: TEV, regular/indirect lookups, alpha
// test and RGB8 or RGBA6 pixel output, consuming the transform stage outputs.
// RGBA6 output uses the compile-time pixel stage of the given plan, dithering by EFB rows
// counted from the top of an efb_height frame. A Z texture with depth writes gl_FragDepth
// (z_texture_glsl) and reads uniform uGxZBias.
std::string gles_fragment_source(const TevPipeline&, const std::string& tev_functions, const IndirectSnapshot&,
                                 std::uint8_t maps, std::uint8_t mip_maps, const PixelPlan* rgba6, bool needs_fetch,
                                 FramebufferFetch, bool early_depth, int efb_height, const ZTextureProgram& = {});

} // namespace starlet::gx
