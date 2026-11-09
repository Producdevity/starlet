#include "starlet/gx/line_point.hpp"

#include <cmath>
#include <cstring>

namespace starlet::gx {

float line_point_texture_offset(unsigned code) {
    static constexpr float offsets[8] = {0.0f, 1.0f / 16, 1.0f / 8, 1.0f / 4, 1.0f / 2, 1.0f, 1.0f, 1.0f};
    return offsets[code & 7];
}

LinePointResult expand_lines_points(std::uint8_t primitive, const std::uint8_t* records, std::uint32_t count,
                                    const StreamFormat& format, const LinePointSetup& setup,
                                    std::vector<std::uint8_t>& vertices, std::vector<std::uint32_t>& indices,
                                    std::uint32_t first_index) {
    LinePointResult result;
    const auto& p = setup.projection;
    const std::size_t stride = format.stride;
    struct Eye { float x, y, z, w; };
    const auto eye = [&](std::uint32_t v) {
        Eye e;
        std::memcpy(&e, records + v * stride, 12);
        e.w = setup.orthographic ? 1.0f : -e.z;
        return e;
    };
    // Screen position in pixels relative to the viewport centre.
    const auto screen = [&](const Eye& e, float& x, float& y) {
        const float cx = p[0] * e.x + (setup.orthographic ? p[1] : p[1] * e.z);
        const float cy = p[2] * e.y + (setup.orthographic ? p[3] : p[3] * e.z);
        x = cx / e.w * setup.viewport_x;
        y = cy / e.w * setup.viewport_y;
    };
    // Copies a record moved by (dx, dy) pixels, with the S and T offsets ds and dt (0 or 1).
    const auto emit = [&](std::uint32_t source, float dx, float dy, float ds, float dt) {
        const auto at = vertices.size();
        vertices.insert(vertices.end(), records + source * stride, records + (source + 1) * stride);
        auto* out = vertices.data() + at;
        const auto e = eye(source);
        const float position[2] = {e.x + dx / setup.viewport_x * e.w / p[0], e.y + dy / setup.viewport_y * e.w / p[2]};
        std::memcpy(out, position, sizeof position);
        if (!ds && !dt) return;
        for (unsigned k = 0; k < 8; ++k) {
            if (!(format.texcoords & (1u << k))) continue;
            const auto& o = setup.offsets[k];
            float st[2];
            std::memcpy(st, out + format.texcoord_offset(k), sizeof st);
            st[0] += ds * o[0] + dt * o[2];
            st[1] += ds * o[1] + dt * o[3];
            std::memcpy(out + format.texcoord_offset(k), st, sizeof st);
        }
    };
    const auto quad = [&] {
        const auto base = first_index + result.vertices;
        for (const std::uint32_t corner : {0u, 1u, 2u, 0u, 2u, 3u}) indices.push_back(base + corner);
        result.vertices += 4;
        result.indices += 6;
    };
    const float half = setup.size / 2;
    if (primitive == 0xb8) {
        for (std::uint32_t v = 0; v < count; ++v) {
            if (!(eye(v).w > 0)) continue;
            emit(v, -half, -half, 0, 0);
            emit(v, half, -half, 1, 0);
            emit(v, half, half, 1, 1);
            emit(v, -half, half, 0, 1);
            quad();
        }
        return result;
    }
    const std::uint32_t step = primitive == 0xa8 ? 2 : 1;
    for (std::uint32_t a = 0; a + 1 < count; a += step) {
        const auto b = a + 1;
        const auto ea = eye(a), eb = eye(b);
        if (!(ea.w > 0 && eb.w > 0)) continue;
        float ax, ay, bx, by;
        screen(ea, ax, ay);
        screen(eb, bx, by);
        const bool tall = std::fabs(by - ay) > std::fabs(bx - ax);
        const float dx = tall ? half : 0, dy = tall ? 0 : half;
        emit(a, -dx, -dy, 0, 0);
        emit(a, dx, dy, 1, 0);
        emit(b, dx, dy, 1, 0);
        emit(b, -dx, -dy, 0, 0);
        quad();
    }
    return result;
}

} // namespace starlet::gx
