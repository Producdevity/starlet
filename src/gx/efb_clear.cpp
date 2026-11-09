#include "starlet/gx/efb_clear.hpp"

#include <algorithm>
#include <stdexcept>

namespace starlet::gx {
EfbClear plan_efb_clear(const State& state, int x, int y, int width, int height,
                       std::array<std::uint8_t, 4> color) {
    const int efb_width = EfbLayout::width, efb_height = state.efb().height;
    if (x < 0 || y < 0 || width <= 0 || height <= 0 || x > efb_width || y > efb_height ||
        width > efb_width - x || height > efb_height - y)
        throw std::invalid_argument("native EFB clear rectangle outside the EFB frame");
    const auto captured = capture_raster(state);
    if (!captured) throw std::runtime_error(captured.status.message);
    const auto& r = *captured.snapshot;
    const auto format = r.pixel_control & 7;
    if (format > 1 || (r.pixel_control & 0x38) || (r.gen_mode & ((1u<<9)|(1u<<19))) ||
        r.quad_offset != std::array<int,2>{{342,342}} || r.strip ||
        r.viewport.left != 0 || r.viewport.top != 0 ||
        r.viewport.width != efb_width || r.viewport.height != efb_height ||
        (r.z_mode & 0x1f) != 0x1f || state.bp(0xf4) != 0 || (state.bp(0xf5) & 15) != 10 ||
        (r.field_mask & 3) != 3 || (r.texture_field_mode & 1) || (r.line_point & (1u<<22)) ||
        (r.fog[0] & (1u<<10)) || (r.fog[9] & (7u<<21)) ||
        (r.color_mode & ((1u<<0)|(1u<<1)|(1u<<11))) || (r.destination_alpha & ~0x1ffu))
        throw std::runtime_error("unsupported native MainLoop EFB clear state");
    EfbClear result;
    result.efb = state.efb();
    const int left = std::max(x, r.scissor.x), bottom = std::max(efb_height-y-height, r.scissor.y);
    const int right = std::min(x+width, r.scissor.x+r.scissor.width);
    const int top = std::min(efb_height-y, r.scissor.y+r.scissor.height);
    result.rectangle = {left,bottom,std::max(0,right-left),std::max(0,top-bottom)};
    result.rgba6 = format == 1;
    result.color_write = r.color_mode & 8;
    result.alpha_write = format && (r.color_mode & 16);
    if (format && (r.destination_alpha & 256)) color[3] = r.destination_alpha & 255;
    constexpr unsigned bayer[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    for (unsigned i=0;i<16;++i) {
        auto& output = result.tile[i];
        for (unsigned c=0;c<4;++c) {
            if (!format) { output[c] = c==3 ? 255 : color[c]; continue; }
            const unsigned value = color[c];
            const unsigned code = c<3 && (r.color_mode & 4) ?
                (((value-(value>>6))<<2)+bayer[i])>>4 : value>>2;
            output[c] = (code<<2)|(code>>4);
        }
    }
    return result;
}
EfbClear plan_copy_clear(const State& state, RasterRect rectangle) {
    EfbClear c;
    c.efb = state.efb();
    c.rectangle = rectangle;
    c.rgba6 = (state.bp(0x43) & 7) == 1;
    c.color_write = state.bp(0x41) & 8;
    c.alpha_write = c.rgba6 && (state.bp(0x41) & 16);
    c.depth_write = state.bp(0x40) & 16;
    c.depth = state.bp(0x51);
    std::array<std::uint8_t, 4> color{{std::uint8_t(state.bp(0x4f)), std::uint8_t(state.bp(0x50) >> 8),
                                      std::uint8_t(state.bp(0x50)), std::uint8_t(state.bp(0x4f) >> 8)}};
    if (c.rgba6) {
        for (auto &channel : color) {
            auto code = channel >> 2;
            channel = (code << 2) | (code >> 4);
        }
    } else
        color[3] = 255;
    c.tile.fill(color);
    return c;
}
} // namespace starlet::gx
