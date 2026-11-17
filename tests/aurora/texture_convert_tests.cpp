#include "texture_convert.hpp"

#include "starlet/gx/texture.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

using namespace starlet::gx;
using aurora::starlet_backend::cmpr_tiled_size;
using aurora::starlet_backend::i8_tiled_size;
using aurora::starlet_backend::rgba8_tiled_size;
using aurora::starlet_backend::tile_bc1_as_cmpr;
using aurora::starlet_backend::tile_i8;
using aurora::starlet_backend::tile_rgba8;

namespace {
void check(bool value, const char* what) { if (!value) throw std::runtime_error(what); }

struct Memory : MemoryReader {
    std::vector<std::uint8_t> image;
    bool copy(std::uint32_t address, std::uint8_t* destination, std::size_t size) override {
        if (address != 0x12000000 || size > image.size()) return false;
        std::copy_n(image.data(), size, destination);
        return true;
    }
};
struct Sink : CommandSink {
    bool draw(const State&, const Draw&, MemoryReader&) override { return true; }
    bool bp_write(const State&, std::uint8_t, std::uint32_t) override { return true; }
    bool invalidate_vertices() override { return true; }
};

// Decodes tiled bytes through Starlet's texture decoder as the given GX format.
std::vector<std::uint8_t> decode(TextureFormat format, unsigned width, unsigned height, std::vector<std::uint8_t> tiled) {
    Memory memory;
    Sink sink;
    Decoder decoder{memory, sink};
    const auto bp = [&](unsigned reg, std::uint32_t value) {
        check(static_cast<bool>(decoder.write_u8(0x61)), "BP opcode");
        check(static_cast<bool>(decoder.write_u32((reg << 24) | value)), "BP payload");
    };
    bp(0x80, 0);
    bp(0x84, 0);
    bp(0x88, (static_cast<unsigned>(format) << 20) | ((height - 1) << 10) | (width - 1));
    bp(0x8c, 0);
    bp(0x90, 0);
    bp(0x94, 0x12000000 >> 5);
    memory.image = std::move(tiled);
    const auto result = capture_texture(decoder.state(), 0, memory, TextureCmprModel::s3tc_candidate_v1, {}, nullptr);
    check(static_cast<bool>(result), result.message.c_str());
    check(result.snapshot->levels().at(0).width == width && result.snapshot->levels()[0].height == height, "decoded size");
    return result.snapshot->levels()[0].rgba;
}

std::vector<std::uint8_t> random_bytes(std::size_t count, unsigned seed) {
    std::mt19937 random(seed);
    std::vector<std::uint8_t> bytes(count);
    for (auto& byte : bytes) byte = static_cast<std::uint8_t>(random());
    return bytes;
}

void rgba8(unsigned width, unsigned height) {
    const auto linear = random_bytes(std::size_t{width} * height * 4, width * 31 + height);
    std::vector<std::uint8_t> tiled(rgba8_tiled_size(width, height));
    tile_rgba8(linear.data(), width, height, tiled.data());
    const auto decoded = decode(TextureFormat::rgba8, width, height, tiled);
    check(decoded == linear, "RGBA8 round trip");
}

void i8(unsigned width, unsigned height) {
    const auto linear = random_bytes(std::size_t{width} * height, width * 17 + height);
    std::vector<std::uint8_t> tiled(i8_tiled_size(width, height));
    tile_i8(linear.data(), width, height, tiled.data());
    const auto decoded = decode(TextureFormat::i8, width, height, tiled);
    for (std::size_t i = 0; i < linear.size(); ++i) {
        check(decoded[i * 4] == linear[i] && decoded[i * 4 + 3] == linear[i], "I8 round trip");
    }
}

using Color = std::array<std::uint8_t, 4>;
std::uint8_t expand5(unsigned n) { return static_cast<std::uint8_t>((n << 3) | (n >> 2)); }
std::uint8_t expand6(unsigned n) { return static_cast<std::uint8_t>((n << 2) | (n >> 4)); }
Color expand565(unsigned n) { return {expand5(n >> 11), expand6((n >> 5) & 63), expand5(n & 31), 255}; }

// One texel of a BC1 block as the S3TC model Starlet's decoder uses gives it.
Color bc1_texel(const std::uint8_t* block, unsigned x, unsigned y) {
    const unsigned a = block[0] | block[1] << 8, b = block[2] | block[3] << 8;
    const Color c0 = expand565(a), c1 = expand565(b);
    const unsigned selector = (block[4 + y] >> (2 * x)) & 3;
    if (selector == 0) return c0;
    if (selector == 1) return c1;
    if (a <= b) {
        if (selector == 3) return {0, 0, 0, 0};
        Color out{0, 0, 0, 255};
        for (unsigned c = 0; c < 3; ++c) out[c] = static_cast<std::uint8_t>((c0[c] + c1[c]) / 2);
        return out;
    }
    Color out{0, 0, 0, 255};
    for (unsigned c = 0; c < 3; ++c) {
        out[c] = static_cast<std::uint8_t>(selector == 2 ? (2u * c0[c] + c1[c]) / 3 : (c0[c] + 2u * c1[c]) / 3);
    }
    return out;
}

void bc1(unsigned width, unsigned height) {
    const unsigned columns = (width + 3) / 4, rows = (height + 3) / 4;
    auto linear = random_bytes(std::size_t{columns} * rows * 8, width * 13 + height);
    // Cover both the four-colour and the three-colour-plus-transparent block kinds.
    for (std::size_t block = 0; block < std::size_t{columns} * rows; ++block) {
        auto* bytes = linear.data() + block * 8;
        if (block % 2 == 0 && (bytes[0] | bytes[1] << 8) <= (bytes[2] | bytes[3] << 8)) std::swap_ranges(bytes, bytes + 2, bytes + 2);
        if (block % 2 == 1 && (bytes[0] | bytes[1] << 8) > (bytes[2] | bytes[3] << 8)) std::swap_ranges(bytes, bytes + 2, bytes + 2);
    }
    std::vector<std::uint8_t> tiled(cmpr_tiled_size(width, height));
    tile_bc1_as_cmpr(linear.data(), width, height, tiled.data());
    const auto decoded = decode(TextureFormat::cmpr, width, height, tiled);
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            const auto expected = bc1_texel(linear.data() + (std::size_t{y / 4} * columns + x / 4) * 8, x % 4, y % 4);
            check(std::equal(expected.begin(), expected.end(), decoded.begin() + (std::size_t{y} * width + x) * 4),
                  "BC1 texel through CMPR");
        }
    }
}
} // namespace

int main() {
    try {
        for (const auto [w, h] : {std::pair{4u, 4u}, {8u, 8u}, {16u, 8u}, {5u, 3u}, {1u, 1u}, {37u, 21u}, {64u, 64u}}) {
            i8(w, h);
            rgba8(w, h);
        }
        for (const auto [w, h] : {std::pair{4u, 4u}, {8u, 8u}, {16u, 8u}, {12u, 20u}, {5u, 3u}, {1u, 1u}, {37u, 21u}, {64u, 64u}}) {
            bc1(w, h);
        }
    } catch (const std::exception& e) {
        std::cerr << "Aurora texture conversion test failed: " << e.what() << '\n';
        return 1;
    }
    std::cout << "Aurora PC texture conversions round-trip through Starlet's decoder\n";
    return 0;
}
