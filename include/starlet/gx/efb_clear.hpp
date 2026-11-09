#pragma once

#include "starlet/gx/raster.hpp"

namespace starlet::gx {

struct EfbClear {
    EfbLayout efb;
    RasterRect rectangle;
    std::array<std::array<std::uint8_t, 4>, 16> tile{};
    bool color_write = false, alpha_write = false, rgba6 = false;
    bool depth_write = true;
    std::uint32_t depth = 0xffffff;
};
// The primary MainLoop clear is an ALWAYS-pass quad replacing Z with 0xffffff.
// Tile entries are RGBA8 storage values in EFB x-right/y-down order. RGBA6 uses
// the existing candidate_v1 alpha reduction and MSB replication convention.
EfbClear plan_efb_clear(const State&, int x, int y, int width, int height,
                       std::array<std::uint8_t, 4> color);
EfbClear plan_copy_clear(const State&, RasterRect);
class EfbClearBackend {
public:
    virtual ~EfbClearBackend() = default;
    virtual void clear_efb(const EfbClear&) = 0;
};

} // namespace starlet::gx
