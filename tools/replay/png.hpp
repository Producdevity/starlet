#pragma once

#include <cstdint>
#include <string>
#include <vector>

// RGBA8 images, top row first.
struct Image {
    unsigned width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};

bool write_png(const std::string& path, const Image& image);
// Reads PNGs written by write_png (8-bit RGBA, no interlace).
bool read_png(const std::string& path, Image& image);
