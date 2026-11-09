#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace starlet::gx {

struct ByteView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

enum class AttributeMode : std::uint8_t { none, direct, index8, index16 };

struct Attribute {
    AttributeMode mode = AttributeMode::none;
    std::uint8_t components = 0;
    std::uint8_t format = 0;
    std::uint8_t fraction_bits = 0;
    std::uint8_t indices = 1;
    std::size_t fifo_bytes = 0;
};

struct VertexLayout {
    std::array<bool, 9> matrix_indices{};
    Attribute position;
    Attribute normal;
    std::array<Attribute, 2> colors{};
    std::array<Attribute, 8> texcoords{};
    bool byte_dequant = false;
    std::size_t stride = 0;
};

struct TexGen {
    bool projected = false;
    bool input_abc1 = false;
    std::uint8_t type = 0;
    std::uint8_t source_row = 0;
    std::uint8_t bump_source = 0;
    std::uint8_t bump_light = 0;
};

// A texture size past the 1024 texels a GX image register holds, which a host gives its own
// textures (a PC port joining a font's pages into one image). It applies while the map's
// register holds the extent's low ten bits in each dimension; zero when unset.
struct TextureExtent {
    std::uint16_t width = 0, height = 0;
    bool operator==(const TextureExtent& o) const { return width == o.width && height == o.height; }
    bool operator!=(const TextureExtent& o) const { return !(*this == o); }
};

struct PaletteBlock {
    std::array<std::uint8_t, 512> bytes{};
    std::uint64_t generation = 0;
    std::uint16_t valid_chunks = 0;
};

// GX's EFB is 640x528 pixels. A render mode draws its frame into the top `height`
// rows (456 for Super Mario Galaxy, 480 for NTSC GameCube modes); the rows below hold
// scratch work such as blur strips, which renderers keep in a separate target.
struct EfbLayout {
    static constexpr int width = 640, rows = 528;
    int height = 456;
    int strip_height() const { return rows - height; }
    bool valid() const { return height > 0 && height <= rows && height % 2 == 0; }
    bool operator==(const EfbLayout& other) const { return height == other.height; }
    bool operator!=(const EfbLayout& other) const { return !(*this == other); }
};

class State {
public:
    explicit State(EfbLayout efb = {});
    static constexpr std::size_t xf_words = 0x1058;
    static constexpr std::size_t palette_bytes = 512 * 1024;

    const EfbLayout& efb() const { return efb_; }
    std::uint32_t bp(std::uint8_t address) const { return bp_[address]; }
    std::uint32_t cp(std::uint8_t address) const { return cp_[address]; }
    // E0..E7 select separate regular/konst banks with BP bit23. half is 0..7.
    std::uint32_t tev_register(bool konst, std::uint8_t half) const;
    std::uint32_t xf(std::uint16_t address) const;
    const std::uint32_t* xf_data() const { return xf_.data(); } // xf_words entries.
    TexGen texgen(std::uint8_t index) const;
    // Effective XF row index: PN=0, TEX0..7=1..8; distinct from raw CP/XF writes.
    std::uint8_t matrix_index(std::uint8_t attribute) const;
    bool vertex_layout(std::uint8_t vat, VertexLayout& layout, std::string& error) const;
    // Offsets are relative to TMEM 0x80000. A State copy retains prior palette bytes.
    bool read_palette(std::size_t offset, std::uint8_t* destination, std::size_t size) const;
    bool palette_generation(std::size_t offset, std::size_t size, std::uint64_t& generation) const;
    std::uint64_t revision() const { return revision_; }
    // Per-category change counters; each change also advances revision().
    std::uint64_t bp_revision() const { return bp_revision_; }
    // BP changes other than regular-bank TEV color registers (E0..E7 with bit23 clear).
    std::uint64_t bp_state_revision() const { return bp_state_revision_; }
    std::uint64_t cp_revision() const { return cp_revision_; }
    // CP changes other than array bases and strides (A0..BF), which no vertex layout reads.
    std::uint64_t cp_layout_revision() const { return cp_layout_revision_; }
    std::uint64_t xf_memory_revision() const { return xf_memory_revision_; } // XF below 0x1000.
    std::uint64_t xf_register_revision() const { return xf_register_revision_; }
    std::uint64_t matrix_index_revision() const { return matrix_index_revision_; }
    // XF memory words [first, end) written by the change that advanced xf_memory_revision() to revision.
    struct XfWrite { std::uint64_t revision = 0; std::uint16_t first = 0, end = 0; };
    XfWrite last_xf_memory_write() const { return last_xf_memory_write_; }
    const TextureExtent& texture_extent(std::uint8_t map) const { return texture_extents_[map & 7]; }

private:
    friend class Decoder;
    EfbLayout efb_;
    std::array<std::uint32_t, 256> bp_{};
    std::array<std::uint32_t, 256> cp_{};
    std::array<std::array<std::uint32_t, 8>, 2> tev_registers_{};
    std::array<std::uint32_t, xf_words> xf_{};
    std::array<std::shared_ptr<const PaletteBlock>, palette_bytes / 512> palette_{};
    std::array<std::uint8_t, 9> matrix_indices_{};
    std::uint32_t bp_mask_ = 0x00ffffff;
    std::uint64_t palette_generation_ = 0;
    std::uint64_t revision_ = 0;
    std::uint64_t bp_revision_ = 0, bp_state_revision_ = 0, cp_revision_ = 0, xf_memory_revision_ = 0, xf_register_revision_ = 0;
    std::uint64_t cp_layout_revision_ = 0, matrix_index_revision_ = 0;
    XfWrite last_xf_memory_write_;
    std::array<TextureExtent, 8> texture_extents_{};
};

} // namespace starlet::gx
