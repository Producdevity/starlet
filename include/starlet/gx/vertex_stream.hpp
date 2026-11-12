#pragma once

#include "starlet/gx/transform.hpp"
#include "starlet/gx/vertex.hpp"

#include <cstring>

namespace starlet::gx {

// Interleaved vertex record holding only the attributes present in a layout, in
// the transform shader's input units: float position (XY layouts store Z=0),
// float normal or NBT, normalized RGBA8 colors, float ST (S-only layouts store
// T=0) and, when any matrix index is explicit, the three packed effective-row
// words PN|TEX0|TEX1, TEX2..4, TEX5..7. Attributes are laid out in that order.
// Batched streams instead hold eye-space positions and end with the four TEV
// registers as signed 16-bit RGBA; append_stream_vertices never writes them.
struct StreamFormat {
    std::uint8_t normal_vectors = 0; // 0, 1 (N) or 3 (NBT).
    std::uint8_t colors = 0;         // Bit per color channel.
    std::uint8_t texcoords = 0;      // Bit per texture coordinate.
    bool matrices = false;
    bool registers = false;
    std::uint16_t stride = 12;
    std::uint16_t normal_offset() const { return 12; }
    std::uint16_t color_offset(unsigned channel) const;
    std::uint16_t texcoord_offset(unsigned coordinate) const;
    std::uint16_t register_offset() const { return static_cast<std::uint16_t>(stride - (registers ? 32 : 0)); }
    std::uint16_t matrix_offset() const { return static_cast<std::uint16_t>(register_offset() - (matrices ? 12 : 0)); }
    bool operator==(const StreamFormat& o) const {
        return normal_vectors == o.normal_vectors && colors == o.colors && texcoords == o.texcoords &&
               matrices == o.matrices && registers == o.registers;
    }
    bool operator!=(const StreamFormat& o) const { return !(*this == o); }
};
StreamFormat stream_format(const VertexLayout&);
// 2^-fraction for a five-bit GX dequantization field; equals ldexp(1, -fraction).
inline float fraction_scale(unsigned fraction) {
    const std::uint32_t bits = (127u - fraction) << 23;
    float scale;
    std::memcpy(&scale, &bits, sizeof scale);
    return scale;
}
// Effective PN/TEX matrix rows packed like the per-vertex words, for draws
// whose layout carries no explicit matrix index; both name the normal matrix by its
// full-form GXMatrices row (transform_normal_row) in the top byte of the first word.
std::array<std::uint32_t, 3> stream_matrix_latches(const State&);

struct StreamResult {
    VertexError error = VertexError::none;
    const char* message = "";
    Topology topology = Topology::triangles;
    std::uint32_t vertices = 0, indices = 0;
    explicit operator bool() const { return error == VertexError::none; }
};
// Byte ranges of each indexed array a load consumed, relative to its CP base.
struct StreamArraySpans {
    // native_unit is the word width read in place; host_order copies MemoryArrayView's.
    struct Span { std::size_t first = 0, end = 0; std::uint8_t native_unit = 0; bool host_order = false; };
    std::array<Span, 12> arrays{};
    // False when an element was read through MemoryReader::copy instead of a view.
    bool viewed = true;
};
// Synchronous inside CommandSink::draw, with the same source contract as
// capture_vertices and bit-identical attribute values. Appends draw.vertex_count
// records (disabled vertices keep zeroed slots) and triangle-list indices offset
// by first_index. Fully disabled primitives are dropped; partially disabled ones,
// lines and points are rejected. On failure both outputs are restored.
StreamResult append_stream_vertices(const State&, const Draw&, MemoryReader&, const StreamFormat&,
                                    std::vector<std::uint8_t>& vertices, std::vector<std::uint32_t>& indices,
                                    std::uint32_t first_index, StreamArraySpans* spans = nullptr);
// The same records for a GX line or point draw, without indices; the caller expands
// them (gx_line_point.hpp). Index-disabled vertices are rejected.
StreamResult append_stream_records(const State&, const Draw&, MemoryReader&, const StreamFormat&,
                                   std::vector<std::uint8_t>& vertices);

} // namespace starlet::gx
