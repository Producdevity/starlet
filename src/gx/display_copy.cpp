#include "starlet/gx/display_copy.hpp"
#include <algorithm>
#include <stdexcept>

namespace starlet::gx {
namespace {
std::uint8_t byte(int n) { return static_cast<std::uint8_t>(std::clamp(n, 0, 255)); }
std::array<int, 3> yuv(std::array<int, 3> rgb) {
    const auto r = rgb[0], g = rgb[1], b = rgb[2];
    return {{(257 * r + 504 * g + 98 * b + 16000) / 1000, (-148 * r - 291 * g + 439 * b + 128000) / 1000,
             (439 * r - 368 * g - 71 * b + 128000) / 1000}};
}
std::array<std::uint8_t, 4> rgb(int y, int u_twice, int v_twice) {
    const auto l = 2 * (y - 16), u = u_twice - 256, v = v_twice - 256;
    return {{byte(std::max(0, 1164 * l + 1596 * v + 1000) / 2000),
             byte(std::max(0, 1164 * l - 392 * u - 813 * v + 1000) / 2000),
             byte(std::max(0, 1164 * l + 2017 * u + 1000) / 2000), 255}};
}
} // namespace
std::size_t display_copy_bytes(const DisplayCopy &p) {
    const int efb_width = EfbLayout::width, efb_height = p.efb.height;
    if (!p.efb.valid() || p.x < 0 || p.y < 0 || p.width <= 0 || p.height <= 0 || p.x > efb_width || p.y > efb_height ||
        p.width > efb_width - p.x || p.height > efb_height - p.y || (p.x & 1) || (p.width & 1) ||
        p.stride < std::size_t(p.width) * 2 || p.stride > 2048 || (p.stride & 31) || p.filter[0] > 126 ||
        p.filter[1] > 189 || p.filter[2] > 126)
        throw std::invalid_argument("invalid native display-copy geometry/filter");
    if ((!p.clamp_top && !p.y && p.filter[0]) ||
        (!p.clamp_bottom && p.y + p.height == efb_height && p.filter[2]))
        throw std::runtime_error("display filter samples outside EFB without clamp");
    return (std::size_t(p.height) - 1) * p.stride + std::size_t(p.width) * 2;
}
DisplayCopy plan_display_copy(const State &state, DisplayCopyRegisters r, bool clear) {
    if ((r.source >> 24) != 0x49 || (r.size >> 24) != 0x4a || (r.stride >> 24) != 0x4d ||
        (r.source & 0x00f00000) || (r.size & 0x00f00000) || (r.stride & 0x00fffc00) || (r.command & ~3u) ||
        state.bp(0x4e) != 256 || (state.bp(0) & (1u << 9)) || (state.bp(0x43) & 7) > 1 ||
        (state.bp(0x43) & 0x38))
        throw std::runtime_error("unsupported native display-copy scale/gamma/field/format");
    DisplayCopy p;
    p.efb = state.efb();
    p.x = r.source & 1023;
    p.y = (r.source >> 10) & 1023;
    p.width = (r.size & 1023) + 1;
    p.height = ((r.size >> 10) & 1023) + 1;
    p.rgba6 = (state.bp(0x43) & 7) == 1;
    p.stride = (r.stride & 1023) * 32;
    p.clamp_top = r.command & 1;
    p.clamp_bottom = r.command & 2;
    const auto a = state.bp(0x53), b = state.bp(0x54);
    p.filter = {{(a & 63) + ((a >> 6) & 63), ((a >> 12) & 63) + ((a >> 18) & 63) + (b & 63),
                 ((b >> 6) & 63) + ((b >> 12) & 63)}};
    display_copy_bytes(p);
    if (clear) p.clear = plan_copy_clear(state,{p.x,p.efb.height-p.y-p.height,p.width,p.height});
    return p;
}
void validate_framebuffer(const FramebufferView &f) {
    if (!f.bytes.data || !f.width || f.width > EfbLayout::width || (f.width & 1) || !f.height ||
        f.height > EfbLayout::rows || f.stride < std::size_t(f.width) * 2 || f.stride > 2048 || (f.stride & 31) ||
        f.bytes.size < (std::size_t(f.height) - 1) * f.stride + std::size_t(f.width) * 2)
        throw std::invalid_argument("invalid native YUYV framebuffer view");
}
std::vector<std::uint8_t> copy_display_reference(const DisplayCopy &p, ByteView rgba) {
    auto size = display_copy_bytes(p);
    const int efb_width = EfbLayout::width, efb_height = p.efb.height;
    if (!rgba.data || rgba.size != std::size_t(efb_width) * efb_height * 4)
        throw std::invalid_argument("invalid reference EFB storage");
    std::vector<std::uint8_t> result(size, 0);
    auto filtered = [&](int x, int y) {
        std::array<int, 3> v{};
        x = std::clamp(x, p.x, p.x + p.width - 1);
        for (int tap = -1; tap <= 1; ++tap) {
            auto row = y + tap;
            if (p.clamp_top)
                row = std::max(row, p.y);
            if (p.clamp_bottom)
                row = std::min(row, p.y + p.height - 1);
            row = std::clamp(row, 0, efb_height - 1);
            auto at = (std::size_t(efb_height - 1 - row) * efb_width + x) * 4;
            for (unsigned c = 0; c < 3; ++c)
                v[c] += rgba.data[at + c] * p.filter[tap + 1];
        }
        for (auto &c : v)
            c = std::min(255, c / 64);
        return yuv(v);
    };
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; x += 2) {
            auto left = filtered(p.x + x - 1, p.y + y), center = filtered(p.x + x, p.y + y),
                 right = filtered(p.x + x + 1, p.y + y);
            const auto at = std::size_t(y) * p.stride + x * 2;
            result[at] = byte(center[0]);
            result[at + 1] = byte((left[1] + 2 * center[1] + right[1]) / 4);
            result[at + 2] = byte(right[0]);
            result[at + 3] = byte((left[2] + 2 * center[2] + right[2]) / 4);
        }
    return result;
}
std::vector<std::uint8_t> decode_framebuffer_reference(const FramebufferView &f) {
    validate_framebuffer(f);
    std::vector<std::uint8_t> result(std::size_t(f.width) * f.height * 4);
    for (unsigned y = 0; y < f.height; ++y)
        for (unsigned x = 0; x < f.width; ++x) {
            auto *p = f.bytes.data + y * f.stride + (x / 2) * 4;
            auto *next = x / 2 + 1 < f.width / 2 ? p + 4 : p;
            auto c =
                rgb(p[(x & 1) * 2], (x & 1) ? p[1] + next[1] : 2 * p[1], (x & 1) ? p[3] + next[3] : 2 * p[3]);
            std::copy(c.begin(), c.end(), result.begin() + (std::size_t(y) * f.width + x) * 4);
        }
    return result;
}
} // namespace starlet::gx
