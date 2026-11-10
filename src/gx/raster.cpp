#include "starlet/gx/raster.hpp"

#include "starlet/gx/depth.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace starlet::gx {
namespace {
float as_float(std::uint32_t word) {
    float value;
    std::memcpy(&value, &word, sizeof value);
    return value;
}
bool integral_int(double value) {
    return std::isfinite(value) && std::trunc(value) == value &&
        value >= std::numeric_limits<int>::min() && value <= std::numeric_limits<int>::max();
}
}

RasterCapture capture_raster(const State& state) {
    auto result = std::make_shared<RasterSnapshot>();
    auto& r = *result;
    for (unsigned i = 0; i < r.viewport.xf.size(); ++i) {
        r.viewport.xf[i] = as_float(state.xf(0x101a + i));
        if (!std::isfinite(r.viewport.xf[i]))
            return {{RasterError::invalid_viewport, "non-finite GX viewport"}, {}};
    }
    const auto offset = state.bp(0x59);
    r.quad_offset = {{int(offset & 0x3ff) * 2, int((offset >> 10) & 0x3ff) * 2}};
    r.scissor_words = {{state.bp(0x20), state.bp(0x21)}};
    const int scissor[4] = {int((r.scissor_words[0] >> 12) & 0x7ff), int(r.scissor_words[0] & 0x7ff),
                            int((r.scissor_words[1] >> 12) & 0x7ff), int(r.scissor_words[1] & 0x7ff)};
    // The quad offset moves pixels to EFB coordinates modulo 2048: FullScreenBlur draws at
    // y - 1584, which lands on row 464. A pixel e is drawn when (e + offset) mod 2048 is
    // inside the scissor; the default offset never reaches the wrapped interval.
    int shift[2];
    for (unsigned axis = 0; axis < 2; ++axis) {
        const int first = scissor[axis], last = scissor[axis + 2], q = r.quad_offset[axis];
        const int extent = axis ? EfbLayout::rows : EfbLayout::width;
        const bool direct = last - q >= 0, wrapped = first - q + 2048 < extent;
        if (direct && wrapped && first <= last)
            return {{RasterError::unsupported_state, "GX quad offset splits the scissor across the coordinate wrap"}, {}};
        shift[axis] = wrapped ? 2048 - q : -q;
    }
    const auto& v = r.viewport.xf;
    r.viewport.left = double(v[3]) - v[0] + shift[0];
    r.viewport.top = double(v[4]) + v[1] + shift[1];
    r.viewport.width = double(v[0]) * 2;
    r.viewport.height = double(v[1]) * -2;
    const int left = scissor[0] + shift[0], top = scissor[1] + shift[1];
    const int right = scissor[2] + 1 + shift[0], bottom = scissor[3] + 1 + shift[1];
    r.efb = state.efb();
    const int x0 = std::clamp(left, 0, EfbLayout::width), x1 = std::clamp(right, 0, EfbLayout::width);
    int y0 = std::clamp(top, 0, EfbLayout::rows), y1 = std::clamp(bottom, 0, EfbLayout::rows);
    r.strip = y0 >= r.efb.height && y1 > y0;
    int height = r.efb.height;
    if (r.strip) {
        y0 -= r.efb.height; y1 -= r.efb.height;
        r.viewport.top -= r.efb.height;
        height = r.efb.strip_height();
    } else {
        y0 = std::min(y0, r.efb.height); y1 = std::min(y1, r.efb.height);
    }
    r.scissor = {x0, height - y1, std::max(0, x1 - x0), std::max(0, y1 - y0)};
    for (unsigned i = 0; i < r.coordinates.size(); ++i) {
        auto& c = r.coordinates[i];
        c.words = {{state.bp(0x30 + i * 2), state.bp(0x31 + i * 2)}};
        c.s = (c.words[0] & 0xffff) + 1;
        c.t = (c.words[1] & 0xffff) + 1;
    }
    r.gen_mode = state.bp(0x00);
    r.line_point = state.bp(0x22);
    r.z_mode = state.bp(0x40);
    r.color_mode = state.bp(0x41);
    r.destination_alpha = state.bp(0x42);
    r.pixel_control = state.bp(0x43);
    r.field_mask = state.bp(0x44);
    r.texture_field_mode = state.bp(0x68);
    r.z_texture = {{state.bp(0xf4),state.bp(0xf5)}};
    for (unsigned i = 0; i < r.fog.size(); ++i) r.fog[i] = state.bp(0xe8 + i);
    r.state_revision = state.revision();
    return {{}, std::move(result)};
}

GlesRasterResult plan_gles_raster(const RasterSnapshot& r, bool rgba6_copy, GuardBand guard_band,
                                  FractionalViewport fractional) {
    auto reject = [](const char* message) {
        return GlesRasterResult{{RasterError::unsupported_state, message}, {}};
    };
    const double gl_y = (r.strip ? r.efb.strip_height() : r.efb.height) - r.viewport.top - r.viewport.height;
    if (fractional == FractionalViewport::round &&
        !(integral_int(r.viewport.left) && integral_int(gl_y) && integral_int(r.viewport.width) &&
          integral_int(r.viewport.height))) {
        auto rounded = r;
        const double left = std::round(r.viewport.left), top = std::round(r.viewport.top);
        rounded.viewport.left = left;
        rounded.viewport.top = top;
        rounded.viewport.width = std::round(r.viewport.left + r.viewport.width) - left;
        rounded.viewport.height = std::round(r.viewport.top + r.viewport.height) - top;
        return plan_gles_raster(rounded, rgba6_copy, guard_band, FractionalViewport::reject);
    }
    if (guard_band == GuardBand::clip_to_viewport && r.scissor.width && r.scissor.height &&
        integral_int(r.viewport.left) && integral_int(gl_y) && integral_int(r.viewport.width) &&
        integral_int(r.viewport.height)) {
        const int left = std::max(r.scissor.x, int(r.viewport.left));
        const int bottom = std::max(r.scissor.y, int(gl_y));
        const int right = std::min(r.scissor.x + r.scissor.width, int(r.viewport.left + r.viewport.width));
        const int top = std::min(r.scissor.y + r.scissor.height, int(gl_y + r.viewport.height));
        if (left != r.scissor.x || bottom != r.scissor.y || right != r.scissor.x + r.scissor.width ||
            top != r.scissor.y + r.scissor.height) {
            auto clipped = r;
            clipped.scissor = {left, bottom, std::max(0, right - left), std::max(0, top - bottom)};
            return plan_gles_raster(clipped, rgba6_copy, GuardBand::reject);
        }
    }
    if (!integral_int(r.viewport.left) || !integral_int(gl_y) ||
        !integral_int(r.viewport.width) || !integral_int(r.viewport.height) ||
        r.viewport.width <= 0 || r.viewport.height <= 0)
        return reject("fractional, reversed, empty or out-of-range GLES viewport");
    if (r.scissor.width && r.scissor.height &&
        (r.scissor.x < r.viewport.left || r.scissor.y < gl_y ||
         double(r.scissor.x) + r.scissor.width > r.viewport.left + r.viewport.width ||
         double(r.scissor.y) + r.scissor.height > gl_y + r.viewport.height))
        return reject("GX guard-band clipping outside the viewport is not implemented");
    if (r.gen_mode & ((1u << 8) | (1u << 9) | (1u << 19)))
        return reject("flat shading, multisampling or Z-freeze is not implemented");
    if ((r.pixel_control & 0x3f) != (rgba6_copy ? 1u : 0u))
        return reject("GLES requires RGB8/RGBA6 with linear Z encoding");
    float far_depth = r.viewport.xf[5]/16777216.0f;
    float near_depth = far_depth-r.viewport.xf[2]/16777216.0f;
    // GX clamps each depth to 24 bits, so a range just past [0,1] (Freezeflame's star
    // select ends at 1.00001) clamps the same; GL clamps the whole range instead.
    constexpr float slack = 1.0f / 256;
    if ((r.z_mode & ~0x1fu) || near_depth<-slack || near_depth>1+slack || far_depth<-slack || far_depth>1+slack)
        return reject("unsupported GX depth mode or viewport depth range");
    near_depth = std::clamp(near_depth, 0.0f, 1.0f);
    far_depth = std::clamp(far_depth, 0.0f, 1.0f);
    if ((r.field_mask & 3) != 3 || (r.texture_field_mode & 1) || (r.line_point & (1u << 22)))
        return reject("interlaced field state is not implemented");
    if ((r.fog[0] & (1u << 10)) || (r.fog[9] & (7u << 21)))
        return reject("GX fog or range adjustment is not implemented");
    if (r.color_mode & (rgba6_copy ? (1u<<1) : ((1u << 1) | (1u << 11))))
        return reject("logic or subtract blend is not implemented");
    if (r.destination_alpha & (rgba6_copy ? 0x600 : 0x700))
        return reject("constant alpha or YUV state is not implemented");
    GlesRasterPlan p;
    p.strip = r.strip;
    p.viewport = {int(r.viewport.left), int(gl_y), int(r.viewport.width), int(r.viewport.height)};
    p.scissor = r.scissor;
    p.blend = !rgba6_copy && (r.color_mode & 1) != 0;
    p.color_write = (r.color_mode & 8) != 0;
    p.cull = (r.gen_mode >> 14) & 3;
    ZTexture z_texture;
    if (!decode_z_texture(r.z_texture[0], r.z_texture[1], z_texture)) return reject("reserved GX Z-texture state");
    p.depth_test = r.z_mode & 1; p.depth_write = r.z_mode & 16;
    p.depth_compare = (r.z_mode >> 1) & 7;
    // Z textures require the depth test after texturing (GX manual 11.2).
    p.early_depth = p.depth_test && (r.pixel_control & 64) && !z_texture.active();
    p.depth_range = {{near_depth,far_depth}};
    p.source_factor = (r.color_mode >> 8) & 7;
    p.destination_factor = (r.color_mode >> 5) & 7;
    if (p.blend && (p.source_factor >= 6 || p.destination_factor >= 6))
        return reject("destination-alpha blend factors are not implemented");
    return {{}, p};
}

GlesRasterResult plan_gles_raster_fast(const RasterSnapshot& r, GuardBand guard_band, FractionalViewport fractional) {
    auto reject = [](const char* message) {
        return GlesRasterResult{{RasterError::unsupported_state, message}, {}};
    };
    const bool rgba6 = (r.pixel_control & 7) == 1;
    RasterSnapshot base = r;
    // Reuse the exact planner for everything but the pixel-format and blend domain.
    base.pixel_control = r.pixel_control & ~7u;
    base.color_mode = r.color_mode & ~((1u << 1) | (1u << 11) | 1u);
    base.destination_alpha = r.destination_alpha & ~0x100u;
    base.fog[0] &= ~(1u << 10);
    base.fog[9] &= ~(7u << 21);
    if ((r.pixel_control & 7) > 1) return reject("fast GLES domain requires RGB8 or RGBA6");
    if (r.destination_alpha & 0x600) return reject("YUV destination state is not implemented");
    if ((r.destination_alpha & 0x100) && !rgba6) return reject("constant alpha requires an alpha EFB");
    auto planned = plan_gles_raster(base, false, guard_band, fractional);
    if (!planned) return planned;
    auto& p = planned.plan;
    p.source_factor = (r.color_mode >> 8) & 7;
    p.destination_factor = (r.color_mode >> 5) & 7;
    p.alpha_write = rgba6 && (r.color_mode & 0x10);
    p.blend_alpha = rgba6;
    if (r.color_mode & (1u << 11)) { p.blend = true; p.subtract = true; p.source_factor = p.destination_factor = 1; }
    else if (r.color_mode & 1) p.blend = true;
    else if (r.color_mode & 2) {
        const auto logic = (r.color_mode >> 12) & 15;
        if (logic == 0) { p.blend = true; p.source_factor = p.destination_factor = 0; }
        else if (logic == 5) { p.color_write = false; p.alpha_write = false; }
        else if (logic != 3) return reject("GX logic operation is not implemented");
    }
    if (p.blend && !rgba6 && (p.source_factor >= 6 || p.destination_factor >= 6))
        return reject("destination-alpha blend factors require an alpha EFB");
    if ((r.destination_alpha & 0x100) && (r.color_mode & ((1u << 11) | 1u))) {
        const bool reads_alpha = !p.subtract && (p.source_factor == 4 || p.source_factor == 5 ||
                                                 p.destination_factor == 4 || p.destination_factor == 5);
        if (!p.alpha_write) p.tev_alpha = reads_alpha;
        else if (!reads_alpha) p.alpha_factors = 1;
        else if (p.destination_factor < 2 || p.destination_factor > 5) {
            p.premultiply = static_cast<std::uint8_t>(p.source_factor - 3);
            p.source_factor = 1;
            p.alpha_factors = 1;
        } else {
            p.tev_alpha = true;
            p.alpha_factors = 2;
        }
    }
    auto& fog = p.fog;
    fog.type = static_cast<std::uint8_t>((r.fog[9] >> 21) & 7);
    if (fog.type == 1 || fog.type == 3) return reject("unknown GX fog type");
    if (fog.type) {
        // a and c are 20-bit floats: the sign, exponent and top 11 mantissa bits of a binary32.
        const auto float20 = [](std::uint32_t v) {
            const std::uint32_t bits = ((v >> 19) & 1) << 31 | ((v >> 11) & 0xff) << 23 | (v & 0x7ff) << 12;
            float f; std::memcpy(&f, &bits, sizeof f); return f;
        };
        fog.orthographic = (r.fog[9] >> 20) & 1;
        fog.parameters = {float20(r.fog[6]), float((r.fog[7] & 0xffffff) / 16777216.0),
                          std::ldexp(1.0f, -int(r.fog[8] & 0x1f)), float20(r.fog[9])};
        fog.color = {((r.fog[10] >> 16) & 0xff) / 255.0f, ((r.fog[10] >> 8) & 0xff) / 255.0f, (r.fog[10] & 0xff) / 255.0f};
        fog.range = (r.fog[0] >> 10) & 1;
        fog.center = float(int(r.fog[0] & 0x3ff) - 342);
        for (unsigned i = 0; i < 10; ++i) fog.table[i] = ((r.fog[1 + i / 2] >> (i % 2 ? 12 : 0)) & 0xfff) / 256.0f;
    }
    return planned;
}

} // namespace starlet::gx
