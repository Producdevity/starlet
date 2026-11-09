#pragma once
#include "starlet/gx/pixel.hpp"

namespace starlet::gx {
// Preserve the same 24 color-storage bits while changing their component widths.
// Input/output are the renderer's expanded RGBA8 storage values, not RGBA6 codes.
PixelColor reinterpret_efb_color(PixelColor, PixelFormat from, PixelFormat to);
std::string_view efb_format_glsl();
}
