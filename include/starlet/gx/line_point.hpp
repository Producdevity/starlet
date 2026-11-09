#pragma once

#include "starlet/gx/vertex_stream.hpp"

namespace starlet::gx {

// GX's setup unit draws lines and points as screen-aligned quads. These helpers
// expand stream records whose positions are already in eye space (batched streams)
// into triangle-list records of the same format.
struct LinePointSetup {
    // XF projection coefficients (0x1020..0x1025) and type (0x1026).
    std::array<float, 6> projection{};
    bool orthographic = false;
    // XF viewport scale (0x101a, 0x101b): half the viewport in EFB pixels. GX's height is negative.
    float viewport_x = 0, viewport_y = 0;
    // Line width or point size in pixels.
    float size = 0;
    // Per input texture coordinate, the change that moves its generated coordinate by
    // the setup offset in S (elements 0 and 1) and in T (2 and 3); zero when not offset.
    std::array<std::array<float, 4>, 8> offsets{};
};
// BP 0x22 and the per-coordinate enables in BP 0x30 + 2n select these offsets, in
// normalized texture coordinates; GX_TO_ZERO is 0.
float line_point_texture_offset(unsigned code);
struct LinePointResult {
    std::uint32_t vertices = 0, indices = 0;
};
// Appends quads for `count` records of `primitive`: 0xA8 lines, 0xB0 a line strip or
// 0xB8 points. A point covers `size` pixels around its vertex with offsets S, T and
// S+T at its right, lower and lower-right corners. A line widens by `size` along
// the screen axis its slope is further from, and the side on the positive axis gets
// the S offset. Primitives with a vertex on or behind the eye plane are dropped.
LinePointResult expand_lines_points(std::uint8_t primitive, const std::uint8_t* records, std::uint32_t count,
                                    const StreamFormat& format, const LinePointSetup& setup,
                                    std::vector<std::uint8_t>& vertices, std::vector<std::uint32_t>& indices,
                                    std::uint32_t first_index);

} // namespace starlet::gx
