#include "starlet/gx/texture.hpp"
#include "starlet/gx/texture_copy.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

using namespace starlet::gx;
namespace {
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
// Copy destinations keep GX's 32-byte alignment; returns the aligned start of bytes.
std::uint8_t* aligned(std::vector<std::uint8_t>& bytes) {
    const auto offset = (32 - reinterpret_cast<std::uintptr_t>(bytes.data()) % 32) % 32;
    return bytes.data() + offset;
}
struct Memory final : MemoryReader {
    std::vector<std::uint8_t> bytes;
    bool copy(std::uint32_t address, std::uint8_t* destination, std::size_t size) override {
        if (address != 0x10000 || size > bytes.size()) return false;
        std::memcpy(destination, bytes.data(), size); return true;
    }
};
struct Sink final : CommandSink {
    bool draw(const State&, const Draw&, MemoryReader&) override { return true; }
    bool bp_write(const State&, std::uint8_t, std::uint32_t) override { return true; }
    bool invalidate_vertices() override { return true; }
};
// Decodes encoded copy bytes through the ordinary texture path.
std::vector<std::uint8_t> sample(const TextureCopy& p, const std::vector<std::uint8_t>& bytes) {
    Memory memory; memory.bytes = bytes; Sink sink; Decoder decoder(memory, sink);
    auto bp = [&](unsigned reg, std::uint32_t value) {
        require(decoder.write_u8(0x61) && decoder.write_u32((reg << 24) | value), "bp");
    };
    bp(0x80, 0); bp(0x84, 0);
    bp(0x88, unsigned(p.destination_width() - 1) | (unsigned(p.destination_height() - 1) << 10) | (texture_copy_sample_format(p) << 20));
    bp(0x94, 0x10000 >> 5);
    const auto captured = capture_texture(decoder.state(), 0, memory);
    require(bool(captured), captured.message);
    return captured.snapshot->levels()[0].rgba;
}
void formats() {
    std::mt19937 random(7);
    unsigned cases = 0;
    for (unsigned format : {1u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u}) for (unsigned variant = 0; variant < 3; ++variant)
    for (bool half : {false, true}) for (bool rgba6 : {false, true}) {
        const bool depth_format = format == 1 || format == 3 || format == 6 || (format >= 8 && format <= 12);
        TextureCopy p;
        p.format = format; p.half_scale = half; p.rgba6 = rgba6;
        p.luminance = format == 1 && variant != 2; p.depth = depth_format && variant == 2;
        p.x = 8; p.y = 4; p.width = 24 + 8 * variant; p.height = 12 + 4 * variant;
        const auto tile_bytes = format == 6 ? 64u : 32u, tile_width = format == 1 || (format >= 7 && format <= 10) ? 8u : 4u;
        p.stride = ((p.destination_width() + tile_width - 1) / tile_width) * tile_bytes + 32 * variant;
        std::vector<std::uint8_t> rgba(std::size_t(p.width) * p.height * 4);
        for (auto& v : rgba) v = static_cast<std::uint8_t>(random());
        if (rgba6) for (auto& v : rgba) v = static_cast<std::uint8_t>((v & 0xfc) | (v >> 6));
        // Depth reads back as its high, middle and low bytes with an opaque alpha.
        if (p.depth) for (std::size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
        const auto size = texture_copy_bytes(p);
        const auto capacity = size + 64;
        std::vector<std::uint8_t> encoded_storage(capacity + 31, 0x5a), stored_storage(capacity + 31, 0x5a);
        auto* encoded = aligned(encoded_storage);
        auto* stored = aligned(stored_storage);
        encode_texture_copy(p, {rgba.data(), rgba.size()}, encoded, capacity);
        const auto decoded = decode_texture_copy(p, {rgba.data(), rgba.size()});
        store_texture_copy(p, {decoded.data(), decoded.size()}, stored, capacity);
        const auto name = "format " + std::to_string(format) + " variant " + std::to_string(variant) +
                          " half " + std::to_string(half) + " rgba6 " + std::to_string(rgba6);
        require(std::memcmp(stored, encoded, capacity) == 0, name + ": stored bytes differ from encoder");
        // Sampling sees the copy as a texture only when its stride is the natural tile-row size.
        if (variant == 0)
            require(sample(p, {encoded, encoded + capacity}) == decoded, name + ": decoded texels differ from texture sampling");
        ++cases;
    }
    require(cases == 132, "case count");
}
// Twilight Princess's half-size depth copy for depth of field and heat haze: GX_TF_Z16 from an RGBA6 EFB.
void z16_plan() {
    Memory memory; Sink sink; Decoder decoder(memory, sink);
    require(decoder.write_u8(0x61) && decoder.write_u32(0x43000041u), "pixel format");
    TextureCopyRegisters r;
    r.source = 0x49000000u; r.size = 0x4a000000u | (447u << 10) | 607u; r.stride = 0x4d000000u | 0x4cu;
    r.command = 0x52010238u; r.depth = true;
    const auto p = plan_texture_copy(decoder.state(), r, false);
    require(p.format == 11 && p.depth && p.half_scale && p.rgba6 && p.destination_width() == 304 && p.destination_height() == 224,
            "Z16 depth copy plan");
    r.command = 0x52010240u; // RGB565 has no depth encoding.
    bool refused = false;
    try { plan_texture_copy(decoder.state(), r, false); } catch (const std::exception&) { refused = true; }
    require(refused, "depth copy to RGB565 is refused");
}
// GX_TF_Z16 copies use PE format 11 (RG8): each texel stores Z's middle byte, then its high byte,
// so sampling the copy as IA8 gives the high byte as intensity.
void z16_layout() {
    TextureCopy p;
    p.format = 11; p.depth = true; p.width = 4; p.height = 4; p.stride = 32;
    std::vector<std::uint8_t> rgba(64);
    for (std::size_t i = 0; i < rgba.size(); i += 4) { rgba[i] = 0x12; rgba[i + 1] = 0x34; rgba[i + 2] = 0x56; rgba[i + 3] = 255; }
    std::vector<std::uint8_t> storage(64 + 31, 0);
    auto* bytes = aligned(storage);
    encode_texture_copy(p, {rgba.data(), rgba.size()}, bytes, 64);
    require(bytes[0] == 0x34 && bytes[1] == 0x12 && bytes[30] == 0x34 && bytes[31] == 0x12, "Z16 copy byte order");
    require(texture_copy_sample_format(p) == 3, "Z16 copy samples as IA8");
    const auto texels = sample(p, {bytes, bytes + 32});
    require(texels[0] == 0x12 && texels[1] == 0x12 && texels[2] == 0x12 && texels[3] == 0x34, "Z16 copy samples high byte as intensity");
}
}

int main() {
    try { formats(); z16_plan(); z16_layout(); }
    catch (const std::exception& error) { std::cerr << "texture_copy_resident_tests: " << error.what() << '\n'; return 1; }
    std::cout << "resident texture-copy model matches encoder and sampler\n";
}
