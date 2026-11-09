#include "starlet/gx/line_point.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace starlet::gx;

namespace {
void require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "gx_line_point: %s\n", message); std::abort(); }
}
bool near(float a, float b) { return std::fabs(a - b) <= 1e-4f * std::max(1.0f, std::fabs(b)); }
struct Vertex { float x, y, z, s, t; };
StreamFormat textured() {
    StreamFormat format;
    format.texcoords = 1;
    format.stride = 20;
    return format;
}
std::vector<std::uint8_t> records(std::initializer_list<Vertex> list) {
    std::vector<std::uint8_t> bytes(list.size() * sizeof(Vertex));
    std::memcpy(bytes.data(), list.begin(), bytes.size());
    return bytes;
}
Vertex at(const std::vector<std::uint8_t>& bytes, unsigned index) {
    Vertex v;
    std::memcpy(&v, bytes.data() + index * sizeof(Vertex), sizeof v);
    return v;
}
// Orthographic: eye x maps to screen pixels right, eye y to pixels up.
LinePointSetup orthographic(float size) {
    LinePointSetup setup;
    setup.projection = {1.0f / 320, 0, 1.0f / 228, 0, -0.001f, -1};
    setup.orthographic = true;
    setup.viewport_x = 320;
    setup.viewport_y = -228;
    setup.size = size;
    setup.offsets[0] = {1, 0, 0, 1};
    return setup;
}
void screen(const LinePointSetup& setup, const Vertex& v, float& x, float& y) {
    const auto& p = setup.projection;
    const float w = setup.orthographic ? 1.0f : -v.z;
    x = (p[0] * v.x + (setup.orthographic ? p[1] : p[1] * v.z)) / w * setup.viewport_x;
    y = (p[2] * v.y + (setup.orthographic ? p[3] : p[3] * v.z)) / w * setup.viewport_y;
}

void points() {
    const auto setup = orthographic(4);
    const auto input = records({{10, 20, -5, 0.25f, 0.5f}});
    std::vector<std::uint8_t> out;
    std::vector<std::uint32_t> indices;
    const auto result = expand_lines_points(0xb8, input.data(), 1, textured(), setup, out, indices, 7);
    require(result.vertices == 4 && result.indices == 6 && out.size() == 4 * sizeof(Vertex), "a point becomes one quad");
    require(indices == std::vector<std::uint32_t>{7, 8, 9, 7, 9, 10}, "quad indices start at the first index");
    const Vertex expected[4] = {{8, 22, -5, 0.25f, 0.5f}, {12, 22, -5, 1.25f, 0.5f}, {12, 18, -5, 1.25f, 1.5f}, {8, 18, -5, 0.25f, 1.5f}};
    for (unsigned i = 0; i < 4; ++i) {
        const auto v = at(out, i);
        require(near(v.x, expected[i].x) && near(v.y, expected[i].y) && v.z == -5 && near(v.s, expected[i].s) &&
                near(v.t, expected[i].t), "point corners: upper left plain, then S, S+T and T offsets");
    }
}
void perspective_points() {
    auto setup = orthographic(6);
    setup.orthographic = false;
    setup.projection = {1.5f, 0.1f, 2.0f, -0.2f, -1.001f, -1};
    const Vertex centre{3, -4, -50, 0, 0};
    const auto input = records({centre, {0, 0, 1, 0, 0}});
    std::vector<std::uint8_t> out;
    std::vector<std::uint32_t> indices;
    const auto result = expand_lines_points(0xb8, input.data(), 2, textured(), setup, out, indices, 0);
    require(result.vertices == 4, "a point behind the eye is dropped");
    float cx, cy;
    screen(setup, centre, cx, cy);
    const float dx[4] = {-3, 3, 3, -3}, dy[4] = {-3, -3, 3, 3};
    for (unsigned i = 0; i < 4; ++i) {
        float x, y;
        screen(setup, at(out, i), x, y);
        require(near(x - cx, dx[i]) && near(y - cy, dy[i]), "perspective corners sit half the size away in pixels");
    }
}
void lines() {
    const auto setup = orthographic(2);
    std::vector<std::uint8_t> out;
    std::vector<std::uint32_t> indices;
    // Wider than tall: widened vertically, the lower side offset in S.
    const auto wide = records({{0, 0, -1, 0, 0}, {100, 10, -1, 0, 1}});
    auto result = expand_lines_points(0xa8, wide.data(), 2, textured(), setup, out, indices, 0);
    require(result.vertices == 4 && result.indices == 6, "a line becomes one quad");
    const Vertex expected[4] = {{0, 1, -1, 0, 0}, {0, -1, -1, 1, 0}, {100, 9, -1, 1, 1}, {100, 11, -1, 0, 1}};
    for (unsigned i = 0; i < 4; ++i) {
        const auto v = at(out, i);
        require(near(v.x, expected[i].x) && near(v.y, expected[i].y) && near(v.s, expected[i].s) && near(v.t, expected[i].t),
                "a wide line spreads up and down with S on its lower side");
    }
    // Taller than wide: widened horizontally, the right side offset in S.
    out.clear(); indices.clear();
    const auto tall = records({{0, 0, -1, 0, 0}, {10, 100, -1, 0, 1}, {10, 200, -1, 0, 2}});
    result = expand_lines_points(0xb0, tall.data(), 3, textured(), setup, out, indices, 0);
    require(result.vertices == 8 && result.indices == 12, "a strip of three vertices draws two quads");
    require(near(at(out, 0).x, -1) && near(at(out, 1).x, 1) && at(out, 1).s == 1 && at(out, 0).s == 0 &&
            near(at(out, 0).y, 0) && near(at(out, 2).y, 100), "a tall line spreads left and right with S on its right side");
    require(indices[6] == 4 && indices[11] == 7, "strip segments continue the index range");
    // A list ignores a trailing odd vertex only through its caller; here two pairs make two quads.
    out.clear(); indices.clear();
    const auto pairs = records({{0, 0, -1, 0, 0}, {5, 0, -1, 0, 0}, {0, 5, -1, 0, 0}, {0, 9, -1, 0, 0}});
    result = expand_lines_points(0xa8, pairs.data(), 4, textured(), setup, out, indices, 0);
    require(result.vertices == 8, "line lists pair their vertices");
}
void offsets() {
    auto setup = orthographic(2);
    setup.offsets[0] = {0, 0, 0, 0};
    const auto input = records({{0, 0, -1, 0.5f, 0.5f}});
    std::vector<std::uint8_t> out;
    std::vector<std::uint32_t> indices;
    expand_lines_points(0xb8, input.data(), 1, textured(), setup, out, indices, 0);
    for (unsigned i = 0; i < 4; ++i) require(at(out, i).s == 0.5f && at(out, i).t == 0.5f, "coordinates without offsets are copied");
    require(line_point_texture_offset(0) == 0 && line_point_texture_offset(1) == 1.0f / 16 && line_point_texture_offset(5) == 1 &&
            line_point_texture_offset(7) == 1, "GX texture offset codes");
}
}

int main() {
    points();
    perspective_points();
    lines();
    offsets();
    std::puts("gx_line_point_tests passed");
}
