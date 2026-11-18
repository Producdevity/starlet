#include "png.hpp"

#include <zlib.h>

#include <cstdio>
#include <cstring>

namespace {
void be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(std::uint8_t(v >> s));
}
std::uint32_t read_be32(const std::uint8_t* p) { return std::uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void chunk(std::vector<std::uint8_t>& out, const char* type, const std::vector<std::uint8_t>& data) {
    be32(out, std::uint32_t(data.size()));
    const auto start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    be32(out, std::uint32_t(crc32(0, out.data() + start, uInt(out.size() - start))));
}
} // namespace

bool write_png(const std::string& path, const Image& image) {
    std::vector<std::uint8_t> raw;
    raw.reserve((image.width * 4 + 1) * image.height);
    for (unsigned y = 0; y < image.height; ++y) {
        raw.push_back(0);
        const auto* row = image.rgba.data() + std::size_t(y) * image.width * 4;
        raw.insert(raw.end(), row, row + image.width * 4);
    }
    uLongf size = compressBound(uLong(raw.size()));
    std::vector<std::uint8_t> packed(size);
    if (compress2(packed.data(), &size, raw.data(), uLong(raw.size()), 6) != Z_OK) return false;
    packed.resize(size);
    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<std::uint8_t> header;
    be32(header, image.width);
    be32(header, image.height);
    header.insert(header.end(), {8, 6, 0, 0, 0});
    chunk(out, "IHDR", header);
    chunk(out, "IDAT", packed);
    chunk(out, "IEND", {});
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    return std::fclose(f) == 0 && ok;
}

bool read_png(const std::string& path, Image& image) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<std::uint8_t> file;
    std::uint8_t buffer[65536];
    for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, f));) file.insert(file.end(), buffer, buffer + n);
    std::fclose(f);
    if (file.size() < 8 || std::memcmp(file.data(), "\x89PNG", 4)) return false;
    std::vector<std::uint8_t> packed;
    for (std::size_t at = 8; at + 12 <= file.size();) {
        const auto length = read_be32(&file[at]);
        if (at + 12 + length > file.size()) return false;
        const char* type = reinterpret_cast<const char*>(&file[at + 4]);
        const auto* data = &file[at + 8];
        if (!std::memcmp(type, "IHDR", 4)) {
            image.width = read_be32(data);
            image.height = read_be32(data + 4);
            if (data[8] != 8 || data[9] != 6 || data[12]) return false;
        } else if (!std::memcmp(type, "IDAT", 4)) {
            packed.insert(packed.end(), data, data + length);
        }
        at += 12 + length;
    }
    const std::size_t stride = std::size_t(image.width) * 4;
    std::vector<std::uint8_t> raw((stride + 1) * image.height);
    uLongf size = uLongf(raw.size());
    if (uncompress(raw.data(), &size, packed.data(), uLong(packed.size())) != Z_OK || size != raw.size()) return false;
    image.rgba.assign(stride * image.height, 0);
    for (unsigned y = 0; y < image.height; ++y) {
        if (raw[y * (stride + 1)] != 0) return false;
        std::memcpy(&image.rgba[y * stride], &raw[y * (stride + 1) + 1], stride);
    }
    return true;
}
