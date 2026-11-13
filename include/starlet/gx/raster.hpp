#pragma once

#include "starlet/gx/state.hpp"

namespace starlet::gx {

struct RasterRect { int x = 0, y = 0, width = 0, height = 0; };
struct RasterViewport {
    std::array<float, 6> xf{}; // sx, sy, sz, ox, oy, oz; original depth units.
    double left = 0, top = 0, width = 0, height = 0;
};
struct RasterScale {
    std::array<std::uint32_t, 2> words{};
    std::uint32_t s = 1, t = 1;
};
struct RasterSnapshot {
    EfbLayout efb;
    RasterViewport viewport; // In the rows of the target the draw lands in.
    RasterRect scissor; // Clipped to that target, GLES lower-left origin.
    // The scissor lies wholly in the EFB rows below the displayed frame.
    bool strip = false;
    std::array<int, 2> quad_offset{};
    std::array<std::uint32_t, 2> scissor_words{};
    std::array<RasterScale, 8> coordinates{};
    std::uint32_t gen_mode = 0, line_point = 0;
    std::uint32_t z_mode = 0, color_mode = 0, destination_alpha = 0, pixel_control = 0;
    std::uint32_t field_mask = 0, texture_field_mode = 0;
    std::array<std::uint32_t,2> z_texture{};
    std::array<std::uint32_t, 11> fog{}; // BP e8..f2, including range coefficients.
    std::uint64_t state_revision = 0;
};
enum class RasterError { none, invalid_viewport, unsupported_state, guard_band };
struct RasterStatus {
    RasterError error = RasterError::none;
    std::string message;
    explicit operator bool() const { return error == RasterError::none; }
};
struct RasterCapture {
    RasterStatus status;
    std::shared_ptr<const RasterSnapshot> snapshot;
    explicit operator bool() const { return static_cast<bool>(status); }
};

// Owns raster registers at the draw boundary. No clipping of the viewport:
// clipping it would change interpolation. Only the scissor intersects the EFB.
// Depth stays in original GX units; this does not assert GLES D24 equivalence.
RasterCapture capture_raster(const State&);

struct GlesRasterPlan {
    RasterRect viewport, scissor;
    bool strip = false; // Renders into the target for EFB rows below the frame.
    bool blend = false, color_write = false, depth_test = false, depth_write = false, early_depth = false;
    std::array<float,2> depth_range{{0,1}};
    std::uint8_t depth_compare = 7;
    std::uint8_t source_factor = 0, destination_factor = 0;
    std::uint8_t cull = 0; // BP encoding: back=1, front=2, both=3.
    // Fast domain only: reverse subtraction, RGBA6 alpha blending and alpha writes.
    bool subtract = false, blend_alpha = false, alpha_write = false;
    // Fast domain only, constant alpha while blending: GX blends colour with the TEV alpha and
    // writes the constant. Without a second fragment output, a source-alpha source factor is
    // applied by the fragment stage (premultiply 1: a, 2: 1-a) and the source factor becomes one;
    // otherwise the fragment keeps TEV alpha (tev_alpha). alpha_factors: 0 as colour, 1 source
    // only, 2 zero (TEV alpha with a destination factor that reads it: exact for constant 0).
    std::uint8_t premultiply = 0, alpha_factors = 0;
    bool tev_alpha = false;
    // Fast domain only: fog after TEV and alpha compare. With z the window depth,
    // x = a/(b - z*shift) - c (perspective) or a*z - c (orthographic); the eye-depth
    // term is scaled by the range table at 32-pixel steps from the center column.
    struct Fog {
        std::uint8_t type = 0; // fsel: 0 none, 2 linear, 4 exp, 5 exp2, 6 reverse exp, 7 reverse exp2.
        bool orthographic = false, range = false;
        std::array<float, 4> parameters{}; // a, b, shift, c
        std::array<float, 3> color{};
        float center = 0;
        std::array<float, 10> table{};
    } fog;
};
struct GlesRasterResult {
    RasterStatus status;
    GlesRasterPlan plan;
    explicit operator bool() const { return static_cast<bool>(status); }
};
// Explicit initial GLES domain: integral viewport, RGB8, smooth
// triangles, GX face culling and linear depth, no field/fog/logic/dither/constant-alpha effects. RGB8 EFB
// alpha is kept opaque and cannot be used as a blend factor in this domain.
// rgba6_copy selects shader pixel arithmetic: blending, masks, forced alpha and
// dither are implemented by that shader, with GL blending disabled. Alpha compare belongs to TEV;
// texture coordinate modifiers are checked by
// the sampling pipeline. Unsupported state is rejected, never dropped.
// GX draws a primitive's pixels outside its viewport, up to the scissor (the guard band); GL
// clips at the viewport. reject refuses a scissor reaching outside the viewport;
// clip_to_viewport draws such primitives clipped at the viewport, dropping those pixels.
enum class GuardBand { reject, clip_to_viewport };
// GL viewports are whole pixels. reject refuses a fractional GX viewport; round moves each
// edge to the nearest pixel, at most half a pixel from where GX maps it.
enum class FractionalViewport { reject, round };
GlesRasterResult plan_gles_raster(const RasterSnapshot&, bool rgba6_copy = false, GuardBand = GuardBand::reject,
                                  FractionalViewport = FractionalViewport::reject);
// Visual-fidelity domain: RGB8 or RGBA6 stored as RGBA8 and blended by GL. RGBA6
// blends all four channels with the GX factors, including destination alpha;
// subtraction is reverse subtraction; dither is not reproduced. Logic operations
// admit copy, no-op and clear only. Constant alpha is written by the fragment stage; see premultiply.
GlesRasterResult plan_gles_raster_fast(const RasterSnapshot&, GuardBand = GuardBand::reject,
                                       FractionalViewport = FractionalViewport::reject);

} // namespace starlet::gx
