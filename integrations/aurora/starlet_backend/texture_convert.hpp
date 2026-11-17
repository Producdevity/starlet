#pragma once

#include <cstddef>
#include <cstdint>

namespace aurora::starlet_backend {

// Converters from Aurora's linear PC texture layouts to GX's tiled big-endian layouts.
// Each writes tiled_size() bytes and repeats the last row and column (or block) into edge tiles.

// GX_TF_R8_PC to I8: 8x4-texel tiles of 32 bytes.
std::size_t i8_tiled_size(std::uint32_t width, std::uint32_t height);
void tile_i8(const std::uint8_t* source, std::uint32_t width, std::uint32_t height, std::uint8_t* tiles);

// GX_TF_RGBA8_PC (R, G, B, A bytes) to RGBA8: 4x4-texel tiles of 64 bytes, the 16 A,R pairs and then the 16 G,B pairs.
std::size_t rgba8_tiled_size(std::uint32_t width, std::uint32_t height);
void tile_rgba8(const std::uint8_t* source, std::uint32_t width, std::uint32_t height, std::uint8_t* tiles);

// GX_TF_BC1_PC (row-major 4x4 blocks of 8 bytes) to CMPR: 8x8-texel tiles of four blocks (top left, top right,
// bottom left, bottom right) with big-endian endpoints and index pairs read from the most significant bits.
std::size_t cmpr_tiled_size(std::uint32_t width, std::uint32_t height);
void tile_bc1_as_cmpr(const std::uint8_t* source, std::uint32_t width, std::uint32_t height, std::uint8_t* tiles);

} // namespace aurora::starlet_backend
