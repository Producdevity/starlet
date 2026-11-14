#include "starlet/gx/state.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace starlet::gx {
namespace {
constexpr auto register_coefficients = [] {
    std::array<std::uint32_t, 278> result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        auto value = (i + 1) * 0x9e3779b97f4a7c15ull;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
        result[i] = static_cast<std::uint32_t>(value ^ (value >> 31)) | 1;
    }
    return result;
}();

struct RegisterMapping {
    std::uint16_t program = 0xffff, draw = 0xffff;
    std::uint32_t mask = ~0u;
};
constexpr auto bp_mapping = [] {
    std::array<RegisterMapping, 256> result{};
    for (unsigned a = 0; a < result.size(); ++a) {
        auto& m = result[a];
        if (a == 0) { m.program = 25; m.mask = ~0xc000u; }
        else if (a >= 0x10 && a < 0x20) m.program = 26 + a - 0x10;
        else if (a >= 0x25 && a < 0x30) m.program = 42 + a - 0x25;
        else if (a >= 0xc0 && a < 0xe0) m.program = 53 + a - 0xc0;
        else if (a == 0xf3) { m.program = 85; m.mask = 0xff0000u; }
        else if (a >= 0xf5 && a < 0xfe) m.program = 86 + a - 0xf5;
        else if (a == 0x43) m.program = 95;
        if (a >= 0x80 && a < 0xc0) m.draw = a;
        else if (a >= 0xe8 && a <= 0xf2) m.draw = a;
        else if (a == 0 || (a >= 0x20 && a <= 0x22) || (a >= 0x30 && a <= 0x44) || a == 0x59 || a == 0x68) {
            m.draw = a;
        } else if ((a >= 6 && a < 0x20) || (a >= 0x25 && a < 0x30) ||
                   (a >= 0xc0 && a < 0xe0) || (a >= 0xf3 && a < 0xfe)) m.draw = a;
    }
    return result;
}();
constexpr auto xf_mapping = [] {
    std::array<RegisterMapping, 88> result{};
    for (unsigned a = 0; a < result.size(); ++a) {
        auto& m = result[a];
        if (a >= 0x1a && a < 0x20) m.draw = 256 + a - 0x1a;
        if (a == 5) m.program = 0;
        else if (a == 9) m.program = 1;
        else if (a >= 0x0e && a <= 0x12) m.program = 2 + a - 0x0e;
        else if (a == 0x26) m.program = 7;
        else if (a == 0x3f) m.program = 8;
        else if (a >= 0x40 && a < 0x48) m.program = 9 + a - 0x40;
        else if (a >= 0x50 && a < 0x58) m.program = 17 + a - 0x50;
    }
    return result;
}();

std::uint32_t bits(std::uint32_t word, unsigned shift, unsigned count) {
    return (word >> shift) & ((1u << count) - 1u);
}

bool attribute_size(Attribute& attribute, bool color, bool normal, std::string& error) {
    if (attribute.mode == AttributeMode::none) {
        return true;
    }
    if ((color && attribute.format > 5) || (!color && attribute.format > 4) ||
        (normal && attribute.format != 1 && attribute.format != 3 && attribute.format != 4)) {
        error = "unsupported vertex component format";
        return false;
    }
    if (attribute.mode == AttributeMode::index8 || attribute.mode == AttributeMode::index16) {
        attribute.fifo_bytes = attribute.indices * (attribute.mode == AttributeMode::index8 ? 1u : 2u);
    } else if (color) {
        constexpr std::size_t sizes[] = {2, 3, 4, 2, 3, 4};
        attribute.fifo_bytes = sizes[attribute.format];
    } else {
        constexpr std::size_t sizes[] = {1, 1, 2, 2, 4};
        attribute.fifo_bytes = attribute.components * sizes[attribute.format];
    }
    return true;
}
}

State::State(EfbLayout efb) : efb_(efb) {
    if (!efb_.valid()) throw std::invalid_argument("EFB frame height outside the 528-row EFB");
    bp_[0xfe] = bp_mask_;
}

void State::program_word(unsigned index, std::uint32_t value) const {
    auto& old = program_registers_[index];
    if (old == value) return;
    program_lanes_[index & 3] += (value - old) * register_coefficients[index];
    old = value;
}

void State::program_bp(std::uint8_t address) const {
    const auto& mapping = bp_mapping[address];
    const auto value = bp_[address];
    if (address >= 0xe0 && address <= 0xe7) {
        // Each bank keeps its own value, so both are read back whichever one the last write hit.
        draw_word(address, tev_registers_[1][address - 0xe0]);
        draw_word(270 + address - 0xe0, tev_registers_[0][address - 0xe0]);
    } else if (mapping.draw != 0xffff) draw_word(mapping.draw, value);
    if (mapping.program != 0xffff) program_word(mapping.program, value & mapping.mask);
}

void State::program_xf(std::uint16_t address) const {
    const auto& mapping = xf_mapping[address - 0x1000];
    const auto value = xf_[address];
    if (mapping.draw != 0xffff) draw_word(mapping.draw, value);
    if (mapping.program != 0xffff) program_word(mapping.program, value);
}

namespace {
// Lane l holds the sum of word * coefficient over indices congruent to l mod 4, in 32-bit arithmetic.
void weighted_lanes(const std::uint32_t* words, unsigned first, unsigned count, std::uint32_t* lanes) {
    std::uint32_t sums[4] = {};
    unsigned i = 0;
    for (; i < count && ((first + i) & 3); ++i) sums[(first + i) & 3] += words[i] * register_coefficients[first + i];
#if defined(__ARM_NEON)
    auto acc = vld1q_u32(sums);
    for (; i + 4 <= count; i += 4) acc = vmlaq_u32(acc, vld1q_u32(words + i), vld1q_u32(register_coefficients.data() + first + i));
    vst1q_u32(sums, acc);
#else
    for (; i + 4 <= count; i += 4)
        for (unsigned l = 0; l < 4; ++l) sums[l] += words[i + l] * register_coefficients[first + i + l];
#endif
    for (; i < count; ++i) sums[(first + i) & 3] += words[i] * register_coefficients[first + i];
    std::memcpy(lanes, sums, sizeof sums);
}
}

// Every key word and hash from the register images: the same result as applying each changed
// register, at a flat cost, for draws that follow many register writes.
void State::rebuild_registers() const {
    static constexpr auto draw_mask = [] {
        std::array<std::uint32_t, 256> mask{};
        for (unsigned a = 0; a < 256; ++a) mask[a] = bp_mapping[a].draw != 0xffff ? ~0u : 0u;
        return mask;
    }();
    auto& w = draw_registers_.words;
    for (unsigned a = 0; a < 256; ++a) w[a] = bp_[a] & draw_mask[a];
    for (unsigned i = 0; i < 8; ++i) {
        w[0xe0 + i] = tev_registers_[1][i];
        w[270 + i] = tev_registers_[0][i];
    }
    for (unsigned i = 0; i < 6; ++i) w[256 + i] = xf_[0x101a + i];
    weighted_lanes(w.data(), 0, 270, draw_registers_.lanes.data());
    weighted_lanes(w.data() + 270, 270, 8, draw_registers_.lanes.data() + 4);
    auto& p = program_registers_;
    p[0] = xf_[0x1005];
    p[1] = xf_[0x1009];
    for (unsigned i = 0; i < 5; ++i) p[2 + i] = xf_[0x100e + i];
    p[7] = xf_[0x1026];
    p[8] = xf_[0x103f];
    for (unsigned i = 0; i < 8; ++i) {
        p[9 + i] = xf_[0x1040 + i];
        p[17 + i] = xf_[0x1050 + i];
    }
    p[25] = bp_[0] & ~0xc000u;
    for (unsigned i = 0; i < 16; ++i) p[26 + i] = bp_[0x10 + i];
    for (unsigned i = 0; i < 11; ++i) p[42 + i] = bp_[0x25 + i];
    for (unsigned i = 0; i < 32; ++i) p[53 + i] = bp_[0xc0 + i];
    p[85] = bp_[0xf3] & 0xff0000u;
    for (unsigned i = 0; i < 9; ++i) p[86 + i] = bp_[0xf5 + i];
    p[95] = bp_[0x43];
    weighted_lanes(p.data(), 0, 96, program_lanes_.data());
}

void State::sync_dirty() const {
    // Registers that no key covers are dropped here, in four mask operations.
    static constexpr auto covered = [] {
        std::array<std::uint64_t, 4> bits{};
        for (unsigned a = 0; a < 256; ++a)
            if (bp_mapping[a].program != 0xffff || bp_mapping[a].draw != 0xffff || (a >= 0xe0 && a <= 0xe7))
                bits[a >> 6] |= std::uint64_t{1} << (a & 63);
        return bits;
    }();
    std::array<std::uint64_t, 4> bp;
    unsigned count = __builtin_popcountll(xf_dirty_[0]) + __builtin_popcountll(xf_dirty_[1]);
    for (unsigned w = 0; w < 4; ++w) {
        bp[w] = bp_dirty_[w] & covered[w];
        bp_dirty_[w] = 0;
        count += __builtin_popcountll(bp[w]);
    }
    // Past a handful of changes the flat rebuild is cheaper than updating each word.
    if (count > 6) {
        xf_dirty_ = {};
        rebuild_registers();
        return;
    }
    for (unsigned w = 0; w < 4; ++w)
        for (auto bits = bp[w]; bits; bits &= bits - 1) program_bp(static_cast<std::uint8_t>(w * 64 + __builtin_ctzll(bits)));
    for (unsigned w = 0; w < 2; ++w) {
        auto bits = xf_dirty_[w];
        xf_dirty_[w] = 0;
        for (; bits; bits &= bits - 1) program_xf(static_cast<std::uint16_t>(0x1000 + w * 64 + __builtin_ctzll(bits)));
    }
}

std::uint64_t State::DrawRegisters::hash(bool batched) const {
    return fold_lanes(lanes.data()) ^ (batched ? 0 : fold_lanes(lanes.data() + 4) * 0xd6e8feb86659fd93ull);
}
bool State::DrawRegisters::matches(const DrawRegisters& other, bool batched) const {
    return !std::memcmp(words.data(), other.words.data(), (batched ? 270 : words.size()) * sizeof(words[0]));
}
void State::draw_word(unsigned index, std::uint32_t value) const {
    auto& old = draw_registers_.words[index];
    if (old == value) return;
    draw_registers_.lanes[(index >= 270 ? 4 : 0) + (index & 3)] += (value - old) * register_coefficients[index];
    old = value;
}
std::uint32_t State::tev_register(bool konst, std::uint8_t half) const {
    if (half >= 8) {
        throw std::out_of_range("GX TEV register half");
    }
    return tev_registers_[konst ? 1 : 0][half];
}

std::uint32_t State::xf(std::uint16_t address) const {
    if (address >= xf_.size()) {
        throw std::out_of_range("GX XF address");
    }
    return xf_[address];
}

std::uint8_t State::matrix_index(std::uint8_t attribute) const {
    if (attribute >= matrix_indices_.size()) {
        throw std::out_of_range("GX matrix index attribute");
    }
    return matrix_indices_[attribute];
}

TexGen State::texgen(std::uint8_t index) const {
    if (index >= 8) {
        throw std::out_of_range("GX texgen index");
    }
    const auto word = xf_[0x1040 + index];
    return {bits(word, 1, 1) != 0, bits(word, 2, 1) != 0,
            static_cast<std::uint8_t>(bits(word, 4, 3)), static_cast<std::uint8_t>(bits(word, 7, 5)),
            static_cast<std::uint8_t>(bits(word, 12, 3)), static_cast<std::uint8_t>(bits(word, 15, 3))};
}

bool State::vertex_layout(std::uint8_t vat, VertexLayout& layout, std::string& error) const {
    layout = {};
    error.clear();
    if (vat >= 8) {
        error = "GX VAT index exceeds seven";
        return false;
    }
    const auto low = cp_[0x50];
    const auto high = cp_[0x60];
    const auto a = cp_[0x70 + vat];
    const auto b = cp_[0x80 + vat];
    const auto c = cp_[0x90 + vat];
    for (unsigned i = 0; i < layout.matrix_indices.size(); ++i) {
        layout.matrix_indices[i] = bits(low, i, 1) != 0;
        layout.stride += layout.matrix_indices[i] ? 1 : 0;
    }
    auto mode = [](std::uint32_t value) { return static_cast<AttributeMode>(value); };
    layout.position = {mode(bits(low, 9, 2)), static_cast<std::uint8_t>(2 + bits(a, 0, 1)),
                       static_cast<std::uint8_t>(bits(a, 1, 3)), static_cast<std::uint8_t>(bits(a, 4, 5)), 1, 0};
    layout.normal = {mode(bits(low, 11, 2)), static_cast<std::uint8_t>(bits(a, 9, 1) ? 9 : 3),
                     static_cast<std::uint8_t>(bits(a, 10, 3)), 0,
                     static_cast<std::uint8_t>(bits(a, 9, 1) && bits(a, 31, 1) ? 3 : 1), 0};
    layout.normal.fraction_bits = layout.normal.format == 1 ? 6 : (layout.normal.format == 3 ? 14 : 0);
    layout.byte_dequant = bits(a, 30, 1) != 0;
    if (!attribute_size(layout.position, false, false, error) || !attribute_size(layout.normal, false, true, error)) {
        return false;
    }
    layout.stride += layout.position.fifo_bytes + layout.normal.fifo_bytes;
    for (unsigned i = 0; i < 2; ++i) {
        auto& color = layout.colors[i];
        color = {mode(bits(low, 13 + i * 2, 2)), static_cast<std::uint8_t>(3 + bits(a, 13 + i * 4, 1)),
                 static_cast<std::uint8_t>(bits(a, 14 + i * 4, 3)), 0, 1, 0};
        if (!attribute_size(color, true, false, error)) {
            return false;
        }
        layout.stride += color.fifo_bytes;
    }
    const std::array<std::uint32_t, 8> descriptors = {
        a >> 21, b, b >> 9, b >> 18, ((b >> 27) & 15u) | ((c & 31u) << 4), c >> 5, c >> 14, c >> 23};
    for (unsigned i = 0; i < 8; ++i) {
        auto& tex = layout.texcoords[i];
        const auto descriptor = descriptors[i];
        tex = {mode(bits(high, i * 2, 2)), static_cast<std::uint8_t>(1 + bits(descriptor, 0, 1)),
               static_cast<std::uint8_t>(bits(descriptor, 1, 3)), static_cast<std::uint8_t>(bits(descriptor, 4, 5)), 1, 0};
        if (!attribute_size(tex, false, false, error)) {
            return false;
        }
        layout.stride += tex.fifo_bytes;
    }
    if (layout.position.mode == AttributeMode::none) {
        error = "draw has no position attribute";
        return false;
    }
    return true;
}

bool State::read_palette(std::size_t offset, std::uint8_t* destination, std::size_t size) const {
    if (offset > palette_bytes || size > palette_bytes - offset || (size && !destination)) {
        return false;
    }
    while (size) {
        const auto& block = palette_[offset / 512];
        const auto count = std::min(size, 512 - offset % 512);
        const auto first_chunk = (offset % 512) / 32;
        const auto last_chunk = ((offset % 512) + std::min(size, 512 - offset % 512) - 1) / 32;
        const auto required = ((1u << (last_chunk - first_chunk + 1)) - 1u) << first_chunk;
        if (!block || (block->valid_chunks & required) != required) {
            return false;
        }
        std::memcpy(destination, block->bytes.data() + offset % 512, count);
        destination += count;
        size -= count;
        offset += count;
    }
    return true;
}

bool State::palette_generation(std::size_t offset, std::size_t size, std::uint64_t& generation) const {
    generation = 0;
    if (offset > palette_bytes || size > palette_bytes - offset) {
        return false;
    }
    while (size) {
        const auto& block = palette_[offset / 512];
        const auto first_chunk = (offset % 512) / 32;
        const auto last_chunk = ((offset % 512) + std::min(size, 512 - offset % 512) - 1) / 32;
        const auto required = ((1u << (last_chunk - first_chunk + 1)) - 1u) << first_chunk;
        if (!block || (block->valid_chunks & required) != required) {
            return false;
        }
        generation = std::max(generation, block->generation);
        const auto count = std::min(size, 512 - offset % 512);
        size -= count;
        offset += count;
    }
    return true;
}

} // namespace starlet::gx
