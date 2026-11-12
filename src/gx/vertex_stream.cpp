#include "starlet/gx/vertex_stream.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace starlet::gx {
namespace {
constexpr std::size_t numeric_size[] = {1, 1, 2, 2, 4};
constexpr std::size_t color_size[] = {2, 3, 4, 2, 3, 4};

std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>((unsigned{p[0]} << 8) | p[1]); }
std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}
bool indexed(const Attribute& a) { return a.mode == AttributeMode::index8 || a.mode == AttributeMode::index16; }
std::uint8_t expand(unsigned value, unsigned width) {
    return static_cast<std::uint8_t>((value << (8 - width)) | (value >> (2 * width - 8)));
}

// One consumed attribute: FIFO bytes for direct data or indices, its array view
// when indexed, and an output slot. Members are assigned before use.
struct Input {
    enum Kind : std::uint8_t { numbers, color } kind;
    std::uint8_t format;
    std::uint8_t index_bytes; // 0 for direct data.
    std::uint8_t vectors;
    std::uint8_t components; // Per vector.
    std::uint8_t width;      // Output floats.
    std::uint8_t unit;       // Native word width this format can read in place.
    std::uint8_t native_unit;
    bool host_order;
    std::uint8_t array;
    std::uint16_t output;
    std::uint32_t fifo_offset; // Within a FIFO vertex record.
    std::uint32_t element_bytes;
    float scale;
    std::uint32_t base, stride;
    const std::uint8_t* data;
    std::size_t size;
};

struct Arrays {
    MemoryReader& memory;
    StreamArraySpans* spans;
    std::array<std::uint8_t, 36> scratch;
    // Returns element bytes and whether they are native-endian words.
    [[gnu::noinline]] const std::uint8_t* read(const Input& in, std::uint16_t element, std::size_t offset, std::size_t size, bool& native) {
        const std::uint64_t relative = std::uint64_t{element} * in.stride + offset;
        // Native units are 2 or 4, so the mask tests divisibility.
        const unsigned native_unit = in.host_order ? in.unit : in.native_unit;
        if (in.data && relative <= in.size && size <= in.size - relative &&
            (!native_unit || (native_unit == in.unit && !((relative | size) & (in.unit - 1u))))) {
            native = native_unit != 0;
            if (spans) {
                auto& span = spans->arrays[in.array];
                if (span.first == span.end) { span.first = relative; span.end = relative + size; }
                else { span.first = std::min<std::size_t>(span.first, relative); span.end = std::max<std::size_t>(span.end, relative + size); }
                span.native_unit = static_cast<std::uint8_t>(native_unit);
                span.host_order = in.host_order;
            }
            return in.data + relative;
        }
        if (spans) spans->viewed = false;
        if (std::uint64_t{in.base} + relative + size > (std::uint64_t{1} << 32) ||
            !memory.copy(static_cast<std::uint32_t>(in.base + relative), scratch.data(), size))
            return nullptr;
        native = false;
        return scratch.data();
    }
};

// One input's array, read in place with only a bounds check per element when every
// element is aligned for its native unit; the consumed range reaches the spans once.
struct InPlace {
    const std::uint8_t* data = nullptr; // Null: every read takes Arrays::read.
    std::size_t size = 0, first = std::numeric_limits<std::size_t>::max(), end = 0;
    bool native = false;
    explicit InPlace(const Input& in) {
        const unsigned native_unit = in.host_order ? in.unit : in.native_unit;
        if (!in.data || (native_unit && (native_unit != in.unit || ((in.stride | in.element_bytes) & (in.unit - 1u))))) return;
        data = in.data;
        size = in.size;
        native = native_unit != 0;
    }
    // Null when the element needs Arrays::read.
    const std::uint8_t* find(const Input& in, std::uint16_t element, std::size_t offset, std::size_t bytes) {
        const std::size_t relative = std::size_t{element} * in.stride + offset;
        if (!data || relative > size || bytes > size - relative) return nullptr;
        first = std::min(first, relative);
        end = std::max(end, relative + bytes);
        return data + relative;
    }
    void finish(const Input& in, StreamArraySpans* spans) const {
        if (!spans || first >= end) return;
        auto& span = spans->arrays[in.array];
        if (span.first == span.end) { span.first = first; span.end = end; }
        else { span.first = std::min(span.first, first); span.end = std::max(span.end, end); }
        span.native_unit = static_cast<std::uint8_t>(in.host_order ? in.unit : in.native_unit);
        span.host_order = in.host_order;
    }
};

template <unsigned Format>
void convert(const std::uint8_t* p, unsigned count, float scale, bool native, float* out) {
    for (unsigned i = 0; i < count; ++i) {
        if constexpr (Format == 0) {
            out[i] = p[i] * scale;
        } else if constexpr (Format == 1) {
            out[i] = static_cast<std::int8_t>(p[i]) * scale;
        } else if constexpr (Format == 2 || Format == 3) {
            std::uint16_t word;
            if (native) std::memcpy(&word, p + i * 2, 2);
            else word = be16(p + i * 2);
            out[i] = (Format == 3 ? int{static_cast<std::int16_t>(word)} : int{word}) * scale;
        } else {
            std::uint32_t word;
            if (native) std::memcpy(&word, p + i * 4, 4);
            else word = be32(p + i * 4);
            std::memcpy(out + i, &word, 4);
        }
    }
}
template <unsigned Format>
void convert_color(const std::uint8_t* p, std::uint8_t* out) {
    if constexpr (Format == 0) { const auto w = be16(p); out[0] = expand(w >> 11, 5); out[1] = expand((w >> 5) & 63, 6); out[2] = expand(w & 31, 5); out[3] = 255; }
    else if constexpr (Format == 1 || Format == 2) { out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255; }
    else if constexpr (Format == 3) { const auto w = be16(p); out[0] = expand(w >> 12, 4); out[1] = expand((w >> 8) & 15, 4); out[2] = expand((w >> 4) & 15, 4); out[3] = expand(w & 15, 4); }
    else if constexpr (Format == 4) { const auto w = (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
        out[0] = expand(w >> 18, 6); out[1] = expand((w >> 12) & 63, 6); out[2] = expand((w >> 6) & 63, 6); out[3] = expand(w & 63, 6); }
    else std::memcpy(out, p, 4);
}

// One input across a draw's vertices; disabled vertices (null when none) are skipped.
struct Column {
    const Input& in;
    const std::uint8_t* fifo;
    std::size_t fifo_stride;
    std::uint8_t* out;
    std::size_t out_stride;
    std::size_t count;
    const std::uint8_t* disabled;
    Arrays& arrays;
};
// Components is the per-vector component count when fixed at compile time, else 0.
template <unsigned IndexBytes, unsigned Format, unsigned Components>
bool numbers_column(const Column& c) {
    const auto& in = c.in;
    const unsigned components = Components ? Components : in.components;
    InPlace array(in);
    for (std::size_t i = 0; i < c.count; ++i) {
        if (c.disabled && c.disabled[i]) continue;
        const auto* cursor = c.fifo + i * c.fifo_stride + in.fifo_offset;
        float values[9];
        unsigned filled = components;
        if constexpr (IndexBytes != 0) {
            for (unsigned v = 0; v < in.vectors; ++v) {
                const std::uint16_t element = IndexBytes == 1 ? cursor[v] : be16(cursor + v * 2);
                bool native = array.native;
                const auto* data = array.find(in, element, v * in.element_bytes, in.element_bytes);
                if (!data && !(data = c.arrays.read(in, element, v * in.element_bytes, in.element_bytes, native))) {
                    array.finish(in, c.arrays.spans);
                    return false;
                }
                convert<Format>(data, components, in.scale, native, values + v * components);
            }
            filled = components * in.vectors;
        } else {
            convert<Format>(cursor, components, in.scale, false, values);
        }
        for (unsigned k = filled; k < in.width; ++k) values[k] = 0;
        auto* target = c.out + i * c.out_stride + in.output;
        switch (in.width) {
        case 2: std::memcpy(target, values, 8); break;
        case 3: std::memcpy(target, values, 12); break;
        case 9: std::memcpy(target, values, 36); break;
        default: std::memcpy(target, values, in.width * 4u); break;
        }
    }
    array.finish(in, c.arrays.spans);
    return true;
}
template <unsigned IndexBytes, unsigned Format>
bool color_column(const Column& c) {
    const auto& in = c.in;
    InPlace array(in);
    for (std::size_t i = 0; i < c.count; ++i) {
        if (c.disabled && c.disabled[i]) continue;
        const auto* data = c.fifo + i * c.fifo_stride + in.fifo_offset;
        if constexpr (IndexBytes != 0) {
            const std::uint16_t element = IndexBytes == 1 ? data[0] : be16(data);
            bool native = false;
            data = array.find(in, element, 0, in.element_bytes);
            if (!data && !(data = c.arrays.read(in, element, 0, in.element_bytes, native))) {
                array.finish(in, c.arrays.spans);
                return false;
            }
        }
        convert_color<Format>(data, c.out + i * c.out_stride + in.output);
    }
    array.finish(in, c.arrays.spans);
    return true;
}
using ColumnFunction = bool (*)(const Column&);
template <unsigned IndexBytes, unsigned Components>
constexpr ColumnFunction numbers_columns[] = {
    numbers_column<IndexBytes, 0, Components>, numbers_column<IndexBytes, 1, Components>, numbers_column<IndexBytes, 2, Components>,
    numbers_column<IndexBytes, 3, Components>, numbers_column<IndexBytes, 4, Components>};
template <unsigned Components>
ColumnFunction numbers_function(const Input& in) {
    return in.index_bytes == 0 ? numbers_columns<0, Components>[in.format]
         : in.index_bytes == 1 ? numbers_columns<1, Components>[in.format] : numbers_columns<2, Components>[in.format];
}
template <unsigned IndexBytes>
constexpr ColumnFunction color_columns[] = {color_column<IndexBytes, 0>, color_column<IndexBytes, 1>, color_column<IndexBytes, 2>,
                                            color_column<IndexBytes, 3>, color_column<IndexBytes, 4>, color_column<IndexBytes, 5>};
ColumnFunction column_function(const Input& in) {
    if (in.kind == Input::color)
        return in.index_bytes == 0 ? color_columns<0>[in.format] : in.index_bytes == 1 ? color_columns<1>[in.format] : color_columns<2>[in.format];
    return in.components == 3 ? numbers_function<3>(in) : in.components == 2 ? numbers_function<2>(in) : numbers_function<0>(in);
}
bool primitive_count(std::uint8_t primitive, std::size_t n, std::size_t& count) {
    switch (primitive) {
    case 0x80: count = (n / 4) * 6; return n % 4 == 0;
    case 0x90: count = n; return n % 3 == 0;
    case 0x98: case 0xa0: count = n >= 3 ? (n - 2) * 3 : 0; return n == 0 || n >= 3;
    default: return false;
    }
}
}

std::uint16_t StreamFormat::color_offset(unsigned channel) const {
    unsigned offset = 12 + normal_vectors * 12u;
    for (unsigned i = 0; i < channel; ++i) if (colors & (1u << i)) offset += 4;
    return static_cast<std::uint16_t>(offset);
}
std::uint16_t StreamFormat::texcoord_offset(unsigned coordinate) const {
    unsigned offset = color_offset(2);
    for (unsigned i = 0; i < coordinate; ++i) if (texcoords & (1u << i)) offset += 8;
    return static_cast<std::uint16_t>(offset);
}

StreamFormat stream_format(const VertexLayout& layout) {
    StreamFormat f;
    if (layout.normal.mode != AttributeMode::none) f.normal_vectors = layout.normal.components == 9 ? 3 : 1;
    for (unsigned i = 0; i < 2; ++i) if (layout.colors[i].mode != AttributeMode::none) f.colors |= 1u << i;
    for (unsigned i = 0; i < 8; ++i) if (layout.texcoords[i].mode != AttributeMode::none) f.texcoords |= 1u << i;
    for (bool explicit_index : layout.matrix_indices) f.matrices |= explicit_index;
    f.stride = f.texcoord_offset(8);
    if (f.matrices) f.stride = static_cast<std::uint16_t>(f.stride + 12);
    return f;
}

std::array<std::uint32_t, 3> stream_matrix_latches(const State& state) {
    std::array<std::uint32_t, 3> words{};
    for (unsigned i = 0; i < 9; ++i)
        words[i / 3] |= std::uint32_t{state.matrix_index(static_cast<std::uint8_t>(i))} << ((i % 3) * 8);
    words[0] |= std::uint32_t{transform_normal_row(state.matrix_index(0))} << 24;
    return words;
}

namespace {
// Appends one record per vertex; index-disabled vertices keep zeroed slots and are marked.
StreamResult convert_records(const State& state, const Draw& draw, MemoryReader& memory, const StreamFormat& format,
                             std::vector<std::uint8_t>& vertices, StreamArraySpans* spans, std::vector<std::uint8_t>& disabled) {
    StreamResult result;
    auto fail = [&](VertexError error, const char* message) { result.error = error; result.message = message; return result; };
    const auto& layout = draw.layout;
    if (stream_format(layout) != format) return fail(VertexError::invalid_layout, "stream format does not match draw layout");
    if ((draw.vertices.size && !draw.vertices.data) || draw.vertices.size != layout.stride * draw.vertex_count)
        return fail(VertexError::invalid_input, "GX draw byte count does not match its vertex layout");
    for (const auto& a : layout.colors)
        if (a.mode != AttributeMode::none && ((a.format < 3) != (a.components == 3)))
            return fail(VertexError::unsupported_layout, "GX color format and component count disagree");

    Input inputs[12];
    unsigned input_count = 0;
    const auto matrix_bytes = static_cast<std::uint32_t>(std::count(layout.matrix_indices.begin(), layout.matrix_indices.end(), true));
    std::uint32_t fifo_offset = matrix_bytes;
    auto add = [&](const Attribute& a, Input::Kind kind, unsigned array, std::uint16_t output) -> Input& {
        auto& in = inputs[input_count++];
        in.fifo_offset = fifo_offset;
        fifo_offset += static_cast<std::uint32_t>(a.fifo_bytes);
        in.kind = kind;
        in.format = a.format;
        in.index_bytes = a.mode == AttributeMode::index8 ? 1 : (a.mode == AttributeMode::index16 ? 2 : 0);
        in.vectors = 1;
        in.components = a.components;
        in.width = 0;
        in.unit = 0;
        in.native_unit = 0;
        in.host_order = false;
        in.array = static_cast<std::uint8_t>(array);
        in.output = output;
        in.element_bytes = 0;
        in.scale = 1;
        in.base = in.stride = 0;
        in.data = nullptr;
        in.size = 0;
        if (in.index_bytes) {
            in.base = state.cp(static_cast<std::uint8_t>(0xa0 + array));
            in.stride = state.cp(static_cast<std::uint8_t>(0xb0 + array)) & 255;
            const auto view = memory.array_view(in.base);
            in.data = view.bytes.data;
            in.size = view.bytes.size;
            in.native_unit = view.native_unit;
            in.host_order = view.host_order;
        }
        return in;
    };
    auto add_numbers = [&](const Attribute& a, unsigned array, bool normal, std::uint16_t output) {
        if (a.mode == AttributeMode::none) return;
        auto& in = add(a, Input::numbers, array, output);
        if (in.index_bytes) in.vectors = a.indices;
        in.components = static_cast<std::uint8_t>(a.components / in.vectors);
        const unsigned fraction = !normal && a.format < 2 && !layout.byte_dequant ? 0 : a.fraction_bits;
        in.scale = fraction_scale(fraction);
        in.element_bytes = static_cast<std::uint32_t>(in.components * numeric_size[a.format]);
        in.width = static_cast<std::uint8_t>(array == 0 ? 3 : (array == 1 ? format.normal_vectors * 3u : 2));
        in.unit = a.format == 4 ? 4 : (a.format == 2 || a.format == 3 ? 2 : 0);
    };
    add_numbers(layout.position, 0, false, 0);
    add_numbers(layout.normal, 1, true, format.normal_offset());
    for (unsigned c = 0; c < 2; ++c) {
        const auto& a = layout.colors[c];
        if (a.mode == AttributeMode::none) continue;
        auto& in = add(a, Input::color, 2 + c, format.color_offset(c));
        in.element_bytes = static_cast<std::uint32_t>(color_size[a.format]);
    }
    for (unsigned t = 0; t < 8; ++t) add_numbers(layout.texcoords[t], 4 + t, false, format.texcoord_offset(t));

    const auto vertex_start = vertices.size();
    vertices.resize(vertex_start + std::size_t{draw.vertex_count} * format.stride);
    auto restore = [&](VertexError error, const char* message) { vertices.resize(vertex_start); return fail(error, message); };
    disabled.clear();
    if (spans) *spans = {};
    Arrays arrays{memory, spans, {}};
    const auto latches = format.matrices ? stream_matrix_latches(state) : std::array<std::uint32_t, 3>{};
    const bool sentinel_possible = indexed(layout.position);
    const unsigned sentinel = layout.position.mode == AttributeMode::index8 ? 255 : 65535;
    if (format.matrices || sentinel_possible) {
        for (std::size_t i = 0; i < draw.vertex_count; ++i) {
            auto* out = vertices.data() + vertex_start + i * format.stride;
            const auto* cursor = draw.vertices.data + i * layout.stride;
            if (format.matrices) {
                std::array<std::uint32_t, 3> words{};
                for (unsigned m = 0; m < 9; ++m) {
                    const std::uint32_t row = layout.matrix_indices[m] ? (*cursor++ & 63u) : ((latches[m / 3] >> ((m % 3) * 8)) & 255u);
                    words[m / 3] |= row << ((m % 3) * 8);
                }
                words[0] |= std::uint32_t{transform_normal_row(words[0] & 63u)} << 24;
                std::memcpy(out + format.matrix_offset(), words.data(), 12);
            }
            if (sentinel_possible) {
                const unsigned position = layout.position.mode == AttributeMode::index8 ? cursor[0] : be16(cursor);
                if (position == sentinel) {
                    if (disabled.empty()) disabled.resize(draw.vertex_count);
                    disabled[i] = 1;
                    std::memset(out, 0, format.matrices ? format.matrix_offset() : format.stride);
                }
            }
        }
    }
    for (unsigned k = 0; k < input_count; ++k) {
        const auto& in = inputs[k];
        const Column column{in, draw.vertices.data, layout.stride, vertices.data() + vertex_start, format.stride,
                            draw.vertex_count, disabled.empty() ? nullptr : disabled.data(), arrays};
        if (!column_function(in)(column))
            return restore(VertexError::memory_read, in.kind == Input::color ? "GX indexed color unavailable" : "GX indexed vertex data unavailable");
    }
    result.vertices = draw.vertex_count;
    return result;
}
}

StreamResult append_stream_vertices(const State& state, const Draw& draw, MemoryReader& memory, const StreamFormat& format,
                                    std::vector<std::uint8_t>& vertices, std::vector<std::uint32_t>& indices,
                                    std::uint32_t first_index, StreamArraySpans* spans) {
    StreamResult result;
    auto fail = [&](VertexError error, const char* message) { result.error = error; result.message = message; return result; };
    const PrimitiveRun single{draw.primitive, draw.vertex_count, 0};
    const PrimitiveRun* runs = draw.merged() ? draw.runs : &single;
    const std::size_t run_count = draw.merged() ? draw.run_count : 1;
    std::size_t index_count = 0;
    for (std::size_t r = 0; r < run_count; ++r) {
        const auto primitive = runs[r].primitive;
        std::size_t count = 0;
        if (primitive == 0xa8 || primitive == 0xb0 || primitive == 0xb8) {
            result.topology = primitive == 0xb8 ? Topology::points : Topology::lines;
            return fail(VertexError::unsupported_layout, "stream renderer admits triangle primitives only");
        }
        if (primitive != 0x80 && primitive != 0x90 && primitive != 0x98 && primitive != 0xa0)
            return fail(VertexError::invalid_primitive, "unsupported GX primitive opcode");
        if (!primitive_count(primitive, runs[r].vertex_count, count))
            return fail(VertexError::partial_primitive, "GX draw has an incomplete primitive");
        index_count += count;
    }
    // A merged draw's runs are converted as one contiguous draw.
    Draw flat = draw;
    thread_local std::vector<std::uint8_t> gathered;
    if (draw.merged()) {
        const auto stride = draw.layout.stride;
        const auto total = draw.total_vertices();
        gathered.resize(std::size_t{total} * stride);
        std::size_t at = 0;
        for (std::size_t r = 0; r < run_count; ++r) {
            const std::size_t bytes = std::size_t{runs[r].vertex_count} * stride;
            if (runs[r].offset > draw.vertices.size || bytes > draw.vertices.size - runs[r].offset)
                return fail(VertexError::invalid_primitive, "GX merged draw run exceeds its vertices");
            std::memcpy(gathered.data() + at, draw.vertices.data + runs[r].offset, bytes);
            at += bytes;
        }
        flat.vertex_count = static_cast<std::uint16_t>(total);
        flat.vertices = {gathered.data(), gathered.size()};
        flat.runs = nullptr;
        flat.run_count = 0;
    }
    const auto vertex_start = vertices.size();
    const auto index_start = indices.size();
    std::vector<std::uint8_t> disabled;
    result = convert_records(state, flat, memory, format, vertices, spans, disabled);
    if (!result) return result;
    auto restore = [&](VertexError error, const char* message) {
        vertices.resize(vertex_start); indices.resize(index_start); return fail(error, message);
    };
    const auto triangles = [&](auto&& emit) {
        std::size_t base = 0;
        for (std::size_t r = 0; r < run_count; ++r) {
            const std::size_t n = runs[r].vertex_count, b = base;
            switch (runs[r].primitive) {
            case 0x80: for (std::size_t i = 0; i < n; i += 4) { emit(b + i, b + i + 1, b + i + 2); emit(b + i, b + i + 2, b + i + 3); } break;
            case 0x90: for (std::size_t i = 0; i < n; i += 3) emit(b + i, b + i + 1, b + i + 2); break;
            case 0x98: for (std::size_t i = 2; i < n; ++i) { if (i % 2 == 0) emit(b + i - 2, b + i - 1, b + i); else emit(b + i - 2, b + i, b + i - 1); } break;
            case 0xa0: for (std::size_t i = 2; i < n; ++i) emit(b, b + i - 1, b + i); break;
            }
            base += n;
        }
    };
    bool partial = false;
    if (disabled.empty()) {
        indices.resize(index_start + index_count);
        auto* out = indices.data() + index_start;
        triangles([&](std::size_t a, std::size_t b, std::size_t c) {
            out[0] = first_index + static_cast<std::uint32_t>(a);
            out[1] = first_index + static_cast<std::uint32_t>(b);
            out[2] = first_index + static_cast<std::uint32_t>(c);
            out += 3;
        });
    } else {
        indices.reserve(index_start + index_count);
        triangles([&](std::size_t a, std::size_t b, std::size_t c) {
            const unsigned off = unsigned(disabled[a]) + disabled[b] + disabled[c];
            if (off) { partial |= off != 3; return; }
            indices.push_back(first_index + static_cast<std::uint32_t>(a));
            indices.push_back(first_index + static_cast<std::uint32_t>(b));
            indices.push_back(first_index + static_cast<std::uint32_t>(c));
        });
    }
    if (partial) return restore(VertexError::partially_disabled_primitive, "GX partially disabled primitive has undefined behavior");
    result.vertices = flat.vertex_count;
    result.indices = static_cast<std::uint32_t>(indices.size() - index_start);
    return result;
}


StreamResult append_stream_records(const State& state, const Draw& draw, MemoryReader& memory, const StreamFormat& format,
                                   std::vector<std::uint8_t>& vertices) {
    StreamResult result;
    if (draw.merged() || (draw.primitive != 0xa8 && draw.primitive != 0xb0 && draw.primitive != 0xb8)) {
        result.error = VertexError::invalid_primitive; result.message = "stream records are for GX lines and points"; return result;
    }
    result.topology = draw.primitive == 0xb8 ? Topology::points : Topology::lines;
    if (draw.primitive == 0xa8 && draw.vertex_count % 2) {
        result.error = VertexError::partial_primitive; result.message = "GX line list has an odd vertex count"; return result;
    }
    const auto start = vertices.size();
    std::vector<std::uint8_t> disabled;
    const auto topology = result.topology;
    result = convert_records(state, draw, memory, format, vertices, nullptr, disabled);
    result.topology = topology;
    if (result && !disabled.empty()) {
        vertices.resize(start);
        result.error = VertexError::partially_disabled_primitive;
        result.message = "GX records with index-disabled vertices are not supported";
        result.vertices = 0;
    }
    return result;
}

} // namespace starlet::gx
