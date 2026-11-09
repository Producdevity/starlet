#pragma once

#include <array>
#include <cstdint>

namespace starlet::gx {

// Parameters of Super Mario Galaxy's two-level bloom (BloomEffect::postDraw): a 4x4 box
// downsample masked by I8 luminance, a 12-tap blur at 160x114, a 36-tap blur of
// that at 80x57, both scaled by intensity and added to the scene.
struct EfbBloom {
    std::uint8_t threshold = 128; // I8 luminance a downsampled texel must exceed.
    std::uint8_t level1_weight = 0, level2_weight = 0; // Per-tap 8-bit TEV rasterized colour.
    std::uint8_t intensity = 0;
    // Texture-coordinate translations of the taps, GX orientation (t grows downwards).
    std::array<std::array<float, 2>, 12> level1{};
    std::array<std::array<float, 2>, 36> level2{};
};

// Host bloom replacing the effect's EFB copies and full-screen passes. begin keeps
// the EFB colour aside and clears it to opaque black, keeping depth; end adds the
// bloom of the colour drawn since then to the kept scene, which becomes the EFB
// colour again. Calls pair up in submission order.
class EfbBloomBackend {
public:
    virtual ~EfbBloomBackend() = default;
    virtual void begin_bloom() = 0;
    virtual void end_bloom(const EfbBloom&) = 0;
};

} // namespace starlet::gx
