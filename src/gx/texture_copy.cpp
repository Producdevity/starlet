#include "starlet/gx/texture_copy.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace starlet::gx {
namespace {
// Copy formats 3, 11 and 12 store two bytes per texel in 4x4 tiles; 1 and 7..10 one byte in 8x4.
unsigned copy_tile_width(unsigned format) { return format == 1 || (format >= 7 && format <= 10) ? 8u : 4u; }
bool two_channel(unsigned format) { return format == 3 || format == 11 || format == 12; }
// Depth copies split Z into high, middle and low bytes and encode them as R, G and B.
bool depth_format(unsigned format) { return format == 1 || format == 3 || format == 6 || (format >= 8 && format <= 12); }
}
std::size_t texture_copy_bytes(const TextureCopy& p) {
    const auto tile_bytes = p.format == 6 ? 64u : 32u;
    const auto tile_width = copy_tile_width(p.format);
    const auto width = p.destination_width(), height = p.destination_height();
    const int efb_width = EfbLayout::width;
    const int first_row = p.strip ? p.efb.height : 0, end_row = p.strip ? EfbLayout::rows : p.efb.height;
    if (p.x < 0 || p.y < first_row || p.width <= 0 || p.height <= 0 || p.x > efb_width || p.y > end_row ||
        p.width > efb_width-p.x || p.height > end_row-p.y || width <= 0 || height <= 0 ||
        (p.strip && (p.depth || p.filtered() || p.clear)) ||
        (p.half_scale && ((p.width & 1) || (p.height & 1))) ||
        (p.format != 1 && (p.format < 3 || p.format > 12)) || (p.depth && !depth_format(p.format)) ||
        (p.format == 1 && !p.depth && !p.luminance) ||
        (p.luminance && (p.depth || p.format != 1)) || (p.stride & 31) ||
        p.stride < std::size_t((width+tile_width-1)/tile_width)*tile_bytes || p.stride > 1023*32)
        throw std::invalid_argument("invalid native texture-copy format="+std::to_string(p.format)+
            " rect="+std::to_string(p.x)+","+std::to_string(p.y)+","+std::to_string(p.width)+","+std::to_string(p.height)+
            " stride="+std::to_string(p.stride));
    return std::size_t((height+3)/4-1)*p.stride+std::size_t((width+tile_width-1)/tile_width)*tile_bytes;
}
TextureCopy plan_texture_copy(const State& state, TextureCopyRegisters r, bool clear) {
    constexpr std::uint32_t allowed = 3u | 0x78u | 0x200u | 0x800u | 0x18000u | 0xff000000u;
    const auto id = r.command >> 24;
    const auto format = ((r.command >> 4) & 7) | ((r.command & 8) ? 8 : 0);
    const auto conversion = (r.command >> 15) & 3;
    if ((r.source >> 24) != 0x49 || (r.size >> 24) != 0x4a || (r.stride >> 24) != 0x4d ||
        (r.source & 0x00f00000) || (r.size & 0x00f00000) || (r.stride & 0x00fffc00) ||
        (r.command & ~allowed) || (id && id != 0x52) ||
        (conversion != 2 && conversion != 3) || (conversion == 3 && (r.depth || format != 1)) ||
        (r.depth && !depth_format(format)) || (state.bp(0) & (1u<<9)) || (state.bp(0x43) & 0x38) ||
        ((state.bp(0x43) & 7) > 1 && !(r.depth && (state.bp(0x43) & 7) == 3)))
        throw std::runtime_error("unsupported native texture-copy AA/depth/scale/field/conversion");
    TextureCopy p;
    p.efb = state.efb();
    p.x = r.source & 1023; p.y = (r.source >> 10) & 1023;
    p.width = (r.size & 1023) + 1; p.height = ((r.size >> 10) & 1023) + 1;
    p.format = format;
    p.depth = r.depth;
    p.luminance = conversion == 3;
    p.stride = (r.stride & 1023)*32;
    p.rgba6 = (state.bp(0x43) & 7) == 1;
    p.half_scale = (r.command & 0x200u) != 0;
    const auto a = state.bp(0x53), b = state.bp(0x54);
    p.filter = {{int((a & 63) + ((a >> 6) & 63)), int(((a >> 12) & 63) + ((a >> 18) & 63) + (b & 63)),
                 int(((b >> 6) & 63) + ((b >> 12) & 63))}};
    p.clamp_top = r.command & 1;
    p.clamp_bottom = r.command & 2;
    p.strip = p.y >= p.efb.height;
    // A rectangle reaching past the frame's right or bottom edge (or past the strip rows) is
    // cut to it, as Dolphin does; the destination keeps only the rows the cut rectangle covers.
    // Pikmin 2's ShadowCylinder2 passes its bottom-right corner as the size.
    {
        const int end_row = p.strip ? EfbLayout::rows : p.efb.height;
        if (p.x < EfbLayout::width && p.x + p.width > EfbLayout::width) p.width = EfbLayout::width - p.x;
        if (p.y < end_row && p.y + p.height > end_row) p.height = end_row - p.y;
        if (p.half_scale) { p.width &= ~1; p.height &= ~1; }
    }
    // Rows below the frame take no filter: the blur's strips are written with it off, and the
    // screen-wobble loops only copy one strip past the frame's last rows without reading it.
    if (p.strip) p.filter = {{0, 64, 0}};
    if (p.strip && clear) throw std::runtime_error("EFB clears below the displayed frame are not implemented");
    texture_copy_bytes(p);
    if (clear) p.clear = plan_copy_clear(state,{p.x,p.efb.height-p.y-p.height,p.width,p.height});
    return p;
}
std::pair<int, int> texture_copy_filter_rows(const TextureCopy& p) {
    return {p.clamp_top ? p.y : std::max(p.y - 1, 0),
            p.clamp_bottom ? p.y + p.height - 1 : std::min(p.y + p.height, p.efb.height - 1)};
}
// Weighted rows are shifted down by six; the sum keeps nine bits, then saturates.
std::vector<std::uint8_t> filter_texture_copy(const TextureCopy& p, ByteView rows, int first) {
    const auto [top, bottom] = texture_copy_filter_rows(p);
    const auto row_bytes = std::size_t(p.width) * 4;
    if (!rows.data || !row_bytes || rows.size % row_bytes) throw std::invalid_argument("invalid texture-copy filter rows");
    const int count = int(rows.size / row_bytes);
    if (first > top || first + count - 1 < bottom) throw std::invalid_argument("texture-copy filter rows miss a neighbour");
    const auto row = [&](int y) { return rows.data + std::size_t(first + count - 1 - y) * row_bytes; };
    std::vector<std::uint8_t> result(row_bytes * p.height);
    for (int r = 0; r < p.height; ++r) {
        const int y = p.y + r;
        const auto *above = row(std::clamp(y - 1, top, bottom)), *at = row(y), *below = row(std::clamp(y + 1, top, bottom));
        auto* out = result.data() + std::size_t(p.height - 1 - r) * row_bytes;
        for (std::size_t x = 0; x < row_bytes; x += 4) {
            for (unsigned c = 0; c < 3; ++c) {
                const int v = ((above[x + c] * p.filter[0] + at[x + c] * p.filter[1] + below[x + c] * p.filter[2]) >> 6) & 0x1ff;
                out[x + c] = std::uint8_t(std::min(v, 255));
            }
            out[x + 3] = at[x + 3];
        }
    }
    return result;
}
void encode_texture_copy(const TextureCopy& p, ByteView rgba, std::uint8_t* destination, std::size_t capacity) {
    const auto required = texture_copy_bytes(p);
    const auto address = reinterpret_cast<std::uintptr_t>(destination);
    if (!destination || (address & 31) || capacity < required || capacity > UINTPTR_MAX-address ||
        !rgba.data || rgba.size != std::size_t(p.width)*p.height*4)
        throw std::invalid_argument("invalid texture-copy storage");
    const auto tile_bytes = p.format == 6 ? 64u : 32u;
    const auto tile_width = copy_tile_width(p.format);
    const auto width = p.destination_width(), height = p.destination_height();
    const unsigned step = p.half_scale ? 2 : 1;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const auto* source = rgba.data+(std::size_t(p.height-1-y*step)*p.width+x*step)*4;
        std::array<unsigned,4> c{{source[0],source[1],source[2],source[3]}};
        if (p.half_scale) {
            const auto* next = source-std::size_t(p.width)*4;
            for (unsigned channel = 0; channel < 4; ++channel)
                c[channel] = (c[channel]+source[4+channel]+next[channel]+next[4+channel])/4;
        }
        const unsigned alpha = p.rgba6 ? c[3] : 255;
        auto* tile = destination+std::size_t(y/4)*p.stride+std::size_t(x/tile_width)*tile_bytes;
        const auto pixel = (y&3)*tile_width+(x%tile_width);
        if (two_channel(p.format)) {
            tile[pixel*2] = p.format == 3 ? alpha : c[p.format == 11 ? 1 : 2];
            tile[pixel*2+1] = c[p.format == 12 ? 1 : 0];
        } else if (p.format == 1 || p.format >= 8) {
            tile[pixel] = p.luminance ? (257*c[0]+504*c[1]+98*c[2]+16000)/1000 : c[p.format == 1 ? 0 : p.format-8];
        } else if (p.format == 7) tile[pixel] = alpha;
        else if (p.format == 6) {
            tile[pixel*2] = alpha; tile[pixel*2+1] = c[0];
            tile[32+pixel*2] = c[1]; tile[33+pixel*2] = c[2];
        } else {
            const unsigned word = p.format == 4 ? (c[0]>>3)<<11 | (c[1]>>2)<<5 | (c[2]>>3) :
                alpha >= 224 ? 0x8000 | (c[0]>>3)<<10 | (c[1]>>3)<<5 | (c[2]>>3) :
                (alpha>>5)<<12 | (c[0]>>4)<<8 | (c[1]>>4)<<4 | (c[2]>>4);
            tile[pixel*2] = word>>8; tile[pixel*2+1] = word;
        }
    }
}
unsigned texture_copy_sample_format(const TextureCopy& p) {
    return p.format == 4 || p.format == 5 || p.format == 6 ? p.format : two_channel(p.format) ? 3 : 1;
}
namespace {
unsigned expand(unsigned value, unsigned bits) { return bits == 4 ? value * 17 : bits == 5 ? (value << 3) | (value >> 2) : (value << 2) | (value >> 4); }
std::size_t tile_offset(const TextureCopy& p, int x, int y, unsigned& pixel) {
    const auto tile_bytes = p.format == 6 ? 64u : 32u;
    const auto tile_width = copy_tile_width(p.format);
    pixel = (y & 3) * tile_width + (x % tile_width);
    return std::size_t(y / 4) * p.stride + std::size_t(x / tile_width) * tile_bytes;
}
}
std::vector<std::uint8_t> decode_texture_copy(const TextureCopy& p, ByteView rgba) {
    texture_copy_bytes(p);
    if (!rgba.data || rgba.size != std::size_t(p.width) * p.height * 4) throw std::invalid_argument("invalid texture-copy source");
    const auto width = p.destination_width(), height = p.destination_height();
    const unsigned step = p.half_scale ? 2 : 1;
    std::vector<std::uint8_t> out(std::size_t(width) * height * 4);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const auto* source = rgba.data + (std::size_t(p.height - 1 - y * step) * p.width + x * step) * 4;
        std::array<unsigned, 4> c{{source[0], source[1], source[2], source[3]}};
        if (p.half_scale) {
            const auto* next = source - std::size_t(p.width) * 4;
            for (unsigned channel = 0; channel < 4; ++channel)
                c[channel] = (c[channel] + source[4 + channel] + next[channel] + next[4 + channel]) / 4;
        }
        const unsigned alpha = p.rgba6 ? c[3] : 255;
        std::array<unsigned, 4> d{};
        if (two_channel(p.format)) {
            const unsigned i = c[p.format == 12 ? 1 : 0], a = p.format == 3 ? alpha : c[p.format == 11 ? 1 : 2];
            d = {{i, i, i, a}};
        } else if (p.format == 1 || p.format >= 7) {
            const unsigned v = p.format == 7 ? alpha : p.luminance ? (257 * c[0] + 504 * c[1] + 98 * c[2] + 16000) / 1000
                             : c[p.format == 1 ? 0 : p.format - 8];
            d = {{v, v, v, v}};
        } else if (p.format == 4) {
            d = {{expand(c[0] >> 3, 5), expand(c[1] >> 2, 6), expand(c[2] >> 3, 5), 255}};
        } else if (p.format == 5) {
            if (alpha >= 224) d = {{expand(c[0] >> 3, 5), expand(c[1] >> 3, 5), expand(c[2] >> 3, 5), 255}};
            else {
                const unsigned a = alpha >> 5;
                d = {{expand(c[0] >> 4, 4), expand(c[1] >> 4, 4), expand(c[2] >> 4, 4), (a << 5) | (a << 2) | (a >> 1)}};
            }
        } else d = {{c[0], c[1], c[2], alpha}};
        for (unsigned k = 0; k < 4; ++k) out[(std::size_t(y) * width + x) * 4 + k] = static_cast<std::uint8_t>(d[k]);
    }
    return out;
}
void store_texture_copy(const TextureCopy& p, ByteView decoded, std::uint8_t* destination, std::size_t capacity) {
    const auto required = texture_copy_bytes(p);
    const auto width = p.destination_width(), height = p.destination_height();
    const auto address = reinterpret_cast<std::uintptr_t>(destination);
    if (!destination || (address & 31) || capacity < required || !decoded.data ||
        decoded.size != std::size_t(width) * height * 4)
        throw std::invalid_argument("invalid texture-copy storage");
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const auto* d = decoded.data + (std::size_t(y) * width + x) * 4;
        unsigned pixel = 0;
        auto* tile = destination + tile_offset(p, x, y, pixel);
        if (two_channel(p.format)) { tile[pixel * 2] = d[3]; tile[pixel * 2 + 1] = d[0]; }
        else if (p.format == 1 || p.format >= 7) tile[pixel] = d[0];
        else if (p.format == 6) {
            tile[pixel * 2] = d[3]; tile[pixel * 2 + 1] = d[0];
            tile[32 + pixel * 2] = d[1]; tile[33 + pixel * 2] = d[2];
        } else {
            const unsigned word = p.format == 4 ? (unsigned(d[0]) >> 3) << 11 | (unsigned(d[1]) >> 2) << 5 | (unsigned(d[2]) >> 3) :
                d[3] == 255 ? 0x8000 | (unsigned(d[0]) >> 3) << 10 | (unsigned(d[1]) >> 3) << 5 | (unsigned(d[2]) >> 3) :
                (unsigned(d[3]) >> 5) << 12 | (unsigned(d[0]) >> 4) << 8 | (unsigned(d[1]) >> 4) << 4 | (unsigned(d[2]) >> 4);
            tile[pixel * 2] = static_cast<std::uint8_t>(word >> 8); tile[pixel * 2 + 1] = static_cast<std::uint8_t>(word);
        }
    }
}
} // namespace starlet::gx
