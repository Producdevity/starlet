#pragma once

#include "starlet/gx/efb_clear.hpp"
#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace starlet::gx {
struct TextureCopyRegisters {
    std::uint32_t source, size, stride, command;
    bool depth;
};
struct TextureCopy {
    EfbLayout efb;
    int x = 0, y = 0, width = 0, height = 0;
    unsigned format = 5;
    std::size_t stride = 0;
    bool rgba6 = false, half_scale = false, depth = false, luminance = false;
    // Weights of the rows above, at and below each source row from the vertical copy
    // filter; 64 is unity. They apply to RGB only. The clamp bits keep the outer rows
    // inside the copied rectangle.
    std::array<int, 3> filter{{0, 64, 0}};
    bool clamp_top = false, clamp_bottom = false;
    // The source lies wholly in the EFB rows below the frame; y still counts from the EFB top.
    bool strip = false;
    bool filtered() const { return filter[0] || filter[1] != 64 || filter[2]; }
    int destination_width() const { return half_scale ? width / 2 : width; }
    int destination_height() const { return half_scale ? height / 2 : height; }
    std::optional<EfbClear> clear;
};
TextureCopy plan_texture_copy(const State&, TextureCopyRegisters, bool clear);
std::size_t texture_copy_bytes(const TextureCopy&);
// Input is the copied rectangle's RGBA8 pixels, with bottom-to-top rows.
// Output is canonical GX tiled storage. Tile-row padding is preserved.
void encode_texture_copy(const TextureCopy&, ByteView rgba, std::uint8_t*, std::size_t capacity);
// The first and last EFB rows (top-based) the vertical filter reads.
std::pair<int, int> texture_copy_filter_rows(const TextureCopy&);
// Applies the vertical filter. Input: EFB rows [first, first + count) of the copied
// columns, bottom-to-top as readback returns them, covering texture_copy_filter_rows.
// Output: the copied rectangle, bottom-to-top, as encode_texture_copy takes it.
std::vector<std::uint8_t> filter_texture_copy(const TextureCopy&, ByteView rows, int first);
// GX texture format code the copy is sampled as: I8 (1) for one-byte outputs,
// otherwise RGB565 (4), RGB5A3 (5) or RGBA8 (6).
unsigned texture_copy_sample_format(const TextureCopy&);
// RGBA8 texels the sampled format decodes from encode_texture_copy's bytes, top row
// first, at destination dimensions. Same input as encode_texture_copy.
std::vector<std::uint8_t> decode_texture_copy(const TextureCopy&, ByteView rgba);
// Inverse of decode_texture_copy: writes the exact encode_texture_copy bytes.
void store_texture_copy(const TextureCopy&, ByteView decoded, std::uint8_t*, std::size_t capacity);
class TextureCopyBackend {
public:
    virtual ~TextureCopyBackend() = default;
    virtual void copy_texture(const TextureCopy&, std::uint8_t*, std::size_t capacity) = 0;
    // token is the destination's GX address. Backends may keep the result
    // GPU-resident; they then own making the bytes visible to later readers.
    virtual void copy_texture_resident(const TextureCopy& copy, std::uint8_t* destination, std::size_t capacity, std::uint32_t) {
        copy_texture(copy, destination, capacity);
    }
    // A writable destination range was unregistered; resident copies of it are dead.
    virtual void release_resident(std::uint32_t, std::size_t) {}
};
} // namespace starlet::gx
