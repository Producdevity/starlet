#include "starlet/gx/efb_format.hpp"
#include <cstdio>
#include <cstdlib>
using namespace starlet::gx;
namespace { void require(bool value) { if (!value) std::abort(); } }
int main() {
    const auto from = PixelFormat::rgb8, to = PixelFormat::rgba6;
    require(reinterpret_efb_color({0,0,0,255},from,to) == PixelColor({0,0,0,0}));
    require(reinterpret_efb_color({255,0,0,255},from,to) == PixelColor({255,195,0,0}));
    require(reinterpret_efb_color({255,255,255,255},from,to) == PixelColor({255,255,255,255}));
    require(reinterpret_efb_color({1,2,3,255},from,from) == PixelColor({1,2,3,255}));
    for (unsigned raw = 0; raw < 1u << 24; ++raw) {
        PixelColor rgb{std::uint8_t(raw >> 16),std::uint8_t(raw >> 8),std::uint8_t(raw),255};
        const auto six = reinterpret_efb_color(rgb,from,to);
        require(reinterpret_efb_color(six,to,from) == rgb);
        for (const auto component : six) require(component == ((component >> 2) << 2 | (component >> 6)));
    }
    std::puts("EFB format: all 16,777,216 storage values round-trip exactly between RGB8 and RGBA6");
}
