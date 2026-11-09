#include "starlet/gx/state.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace starlet::gx {
namespace {
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
