#include "texture_convert.hpp"

#include <algorithm>
#include <cstring>

namespace aurora::starlet_backend {
namespace {
std::uint32_t blocks(std::uint32_t texels, std::uint32_t tile) { return (texels + tile - 1) / tile; }
} // namespace

std::size_t i8_tiled_size(std::uint32_t width, std::uint32_t height) {
  return std::size_t{blocks(width, 8)} * blocks(height, 4) * 32;
}

// Copies linear 8-bit texels into GX I8 layout: 8x4-texel tiles of 32 bytes, left to right, then
// down. Edge tiles repeat the last row and column.
void tile_i8(const std::uint8_t* source, std::uint32_t width, std::uint32_t height, std::uint8_t* tiles) {
  for (std::uint32_t tile_y = 0; tile_y < blocks(height, 4); ++tile_y) {
    for (std::uint32_t x = 0; x < width; x += 8) {
      const std::uint32_t count = std::min<std::uint32_t>(8, width - x);
      for (std::uint32_t row = 0; row < 4; ++row, tiles += 8) {
        const auto* line = source + std::size_t{std::min(tile_y * 4 + row, height - 1)} * width;
        std::memcpy(tiles, line + x, count);
        std::memset(tiles + count, line[width - 1], 8 - count);
      }
    }
  }
}

std::size_t rgba8_tiled_size(std::uint32_t width, std::uint32_t height) {
  return std::size_t{blocks(width, 4)} * blocks(height, 4) * 64;
}

void tile_rgba8(const std::uint8_t* source, std::uint32_t width, std::uint32_t height, std::uint8_t* tiles) {
  for (std::uint32_t tile_y = 0; tile_y < blocks(height, 4); ++tile_y) {
    for (std::uint32_t tile_x = 0; tile_x < blocks(width, 4); ++tile_x, tiles += 64) {
      for (std::uint32_t row = 0; row < 4; ++row) {
        const std::size_t y = std::min(tile_y * 4 + row, height - 1);
        for (std::uint32_t column = 0; column < 4; ++column) {
          const std::size_t x = std::min(tile_x * 4 + column, width - 1);
          const auto* texel = source + (y * width + x) * 4;
          auto* pair = tiles + (row * 4 + column) * 2;
          pair[0] = texel[3];
          pair[1] = texel[0];
          pair[32] = texel[1];
          pair[33] = texel[2];
        }
      }
    }
  }
}

std::size_t cmpr_tiled_size(std::uint32_t width, std::uint32_t height) {
  return std::size_t{blocks(width, 8)} * blocks(height, 8) * 32;
}

void tile_bc1_as_cmpr(const std::uint8_t* source, std::uint32_t width, std::uint32_t height, std::uint8_t* tiles) {
  const std::uint32_t block_columns = blocks(width, 4), block_rows = blocks(height, 4);
  for (std::uint32_t tile_y = 0; tile_y < blocks(height, 8); ++tile_y) {
    for (std::uint32_t tile_x = 0; tile_x < blocks(width, 8); ++tile_x) {
      for (std::uint32_t sub = 0; sub < 4; ++sub, tiles += 8) {
        const std::uint32_t column = std::min(tile_x * 2 + sub % 2, block_columns - 1);
        const std::uint32_t row = std::min(tile_y * 2 + sub / 2, block_rows - 1);
        const auto* block = source + (std::size_t{row} * block_columns + column) * 8;
        tiles[0] = block[1];
        tiles[1] = block[0];
        tiles[2] = block[3];
        tiles[3] = block[2];
        for (std::uint32_t line = 0; line < 4; ++line) {
          const std::uint8_t bits = block[4 + line];
          tiles[4 + line] = static_cast<std::uint8_t>((bits & 3) << 6 | (bits & 0x0c) << 2 | (bits >> 2 & 0x0c) | bits >> 6);
        }
      }
    }
  }
}

} // namespace aurora::starlet_backend
