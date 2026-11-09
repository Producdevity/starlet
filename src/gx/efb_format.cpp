#include "starlet/gx/efb_format.hpp"

namespace starlet::gx {
PixelColor reinterpret_efb_color(PixelColor color, PixelFormat from, PixelFormat to) {
    if (from == to) return color;
    const unsigned source_count = from == PixelFormat::rgba6 ? 4 : 3;
    const unsigned source_bits = 24 / source_count;
    std::uint32_t storage = 0;
    for (unsigned channel = 0; channel < source_count; ++channel)
        storage = (storage << source_bits) | (color[channel] >> (8 - source_bits));
    const unsigned target_count = to == PixelFormat::rgba6 ? 4 : 3;
    const unsigned target_bits = 24 / target_count;
    PixelColor result{0,0,0,255};
    for (unsigned channel = target_count; channel-- > 0;) {
        const auto code = storage & ((1u << target_bits) - 1);
        result[channel] = target_bits == 8 ? code : (code << 2) | (code >> 4);
        storage >>= target_bits;
    }
    return result;
}
std::string_view efb_format_glsl() {
    return R"GLSL(
uvec4 gx_reinterpret_efb(uvec4 color, bool fromRgba6) {
    uint sourceCount = fromRgba6 ? 4u : 3u;
    uint sourceBits = 24u / sourceCount;
    uint word = 0u;
    for (uint c = 0u; c < sourceCount; ++c)
        word = (word << sourceBits) | (color[c] >> (8u - sourceBits));
    uint targetCount = 7u - sourceCount;
    uint targetBits = 24u / targetCount;
    uvec4 result = uvec4(0u,0u,0u,255u);
    for (int c = int(targetCount) - 1; c >= 0; --c) {
        uint code = word & ((1u << targetBits) - 1u);
        result[c] = targetBits == 8u ? code : (code << 2u) | (code >> 4u);
        word >>= targetBits;
    }
    return result;
}
)GLSL";
}
}
