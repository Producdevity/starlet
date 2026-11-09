#include "starlet/gx/vertex_stream.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>

using namespace starlet::gx;
namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
struct Memory final : MemoryReader {
    std::map<std::uint32_t, std::vector<std::uint8_t>> regions;
    std::map<std::uint32_t, unsigned> native_units;
    std::map<std::uint32_t, bool> host_order;
    bool array_views = true;
    MemoryArrayView array_view(std::uint32_t address) override {
        if (!array_views) return {};
        auto it = regions.upper_bound(address);
        if (it == regions.begin()) return {};
        --it;
        const auto offset = std::size_t{address - it->first};
        const auto unit = native_units[it->first];
        if (offset >= it->second.size() || (unit && offset % unit)) return {};
        MemoryArrayView view{{it->second.data() + offset, it->second.size() - offset}, static_cast<std::uint8_t>(unit)};
        view.host_order = host_order[it->first];
        return view;
    }
    bool copy(std::uint32_t address, std::uint8_t* destination, std::size_t size) override {
        auto it = regions.upper_bound(address);
        if (it == regions.begin()) return false;
        --it;
        const auto offset = static_cast<std::size_t>(address - it->first);
        if (offset > it->second.size() || size > it->second.size() - offset) return false;
        const auto unit = native_units[it->first];
        for (std::size_t i = 0; i < size; ++i) {
            if (!unit) { destination[i] = it->second[offset + i]; continue; }
            const auto position = offset + i, start = position - position % unit;
            std::uint32_t word;
            if (unit == 2) { std::uint16_t half; std::memcpy(&half, it->second.data() + start, 2); word = half; }
            else std::memcpy(&word, it->second.data() + start, 4);
            destination[i] = static_cast<std::uint8_t>(word >> (8 * (unit - 1 - position % unit)));
        }
        return true;
    }
};
struct Sink final : CommandSink {
    bool draw(const State&, const Draw&, MemoryReader&) override { return true; }
    bool bp_write(const State&, std::uint8_t, std::uint32_t) override { return true; }
    bool invalidate_vertices() override { return true; }
};
struct Fixture {
    Memory memory;
    Sink sink;
    Decoder decoder{memory, sink};
    void cp(std::uint8_t address, std::uint32_t value) {
        require(static_cast<bool>(decoder.write_u8(8)) && decoder.write_u8(address) && decoder.write_u32(value), "cp write");
    }
    void xf(std::uint16_t address, std::uint32_t value) {
        require(decoder.write_u8(0x10) && decoder.write_u16(0) && decoder.write_u16(address) && decoder.write_u32(value), "xf write");
    }
};

void compare(Fixture& f, const Draw& draw, std::uint32_t first_index, const std::string& name) {
    const auto reference = capture_vertices(f.decoder.state(), draw, f.memory);
    const auto format = stream_format(draw.layout);
    std::vector<std::uint8_t> vertices(7, 0xcd);
    std::vector<std::uint32_t> indices(3, 9);
    const auto result = append_stream_vertices(f.decoder.state(), draw, f.memory, format, vertices, indices, first_index);
    require(bool(reference) == bool(result), name + ": acceptance differs: " + reference.message + " / " + result.message);
    if (!result) {
        require(vertices.size() == 7 && indices.size() == 3, name + ": failure did not restore outputs");
        return;
    }
    const auto& expected = reference.snapshot->vertices();
    require(result.vertices == expected.size() && vertices.size() == 7 + expected.size() * format.stride, name + ": vertex count");
    const auto latches = stream_matrix_latches(f.decoder.state());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& v = expected[i];
        if (v.disabled) continue;
        const auto* out = vertices.data() + 7 + i * format.stride;
        require(!std::memcmp(out, v.position.data(), 12), name + ": position " + std::to_string(i));
        if (format.normal_vectors)
            require(!std::memcmp(out + format.normal_offset(), v.normal.data(), format.normal_vectors * 12u), name + ": normal");
        for (unsigned c = 0; c < 2; ++c) if (format.colors & (1u << c))
            require(!std::memcmp(out + format.color_offset(c), v.colors[c].data(), 4), name + ": color");
        for (unsigned t = 0; t < 8; ++t) if (format.texcoords & (1u << t))
            require(!std::memcmp(out + format.texcoord_offset(t), v.texcoords[t].data(), 8), name + ": texcoord");
        std::array<std::uint32_t, 3> words{};
        for (unsigned m = 0; m < 9; ++m) words[m / 3] |= std::uint32_t{v.matrix_indices[m]} << ((m % 3) * 8);
        if (format.matrices) require(!std::memcmp(out + format.matrix_offset(), words.data(), 12), name + ": matrices");
        else require(words == latches, name + ": matrix latches");
    }
    const auto& reference_indices = reference.snapshot->indices();
    require(result.indices == reference_indices.size() && indices.size() == 3 + reference_indices.size(), name + ": index count");
    for (std::size_t i = 0; i < reference_indices.size(); ++i)
        require(indices[3 + i] == first_index + reference_indices[i], name + ": index value");
}

void randomized() {
    std::mt19937 random(0x5eed);
    auto pick = [&](unsigned n) { return static_cast<unsigned>(random() % n); };
    unsigned compared = 0;
    for (unsigned trial = 0; trial < 4000; ++trial) {
        Fixture f;
        std::uint32_t low = 0, high = 0, a = 0, b = 0, c = 0;
        for (unsigned m = 0; m < 9; ++m) if (pick(4) == 0) low |= 1u << m;
        low |= (1 + pick(3)) << 9;
        a |= pick(2) | (pick(5) << 1) | (pick(32) << 4);
        if (pick(2)) {
            low |= (1 + pick(3)) << 11;
            const unsigned formats[] = {1, 3, 4};
            a |= (pick(2) << 9) | (formats[pick(3)] << 10) | (pick(2) << 31);
        }
        for (unsigned ch = 0; ch < 2; ++ch) if (pick(2)) {
            low |= (1 + pick(3)) << (13 + ch * 2);
            const unsigned format = pick(6);
            a |= ((format < 3 ? 0u : 1u) << (13 + ch * 4)) | (format << (14 + ch * 4));
        }
        if (pick(3) == 0) a |= 1u << 30;
        std::array<std::uint32_t, 8> descriptors{};
        for (unsigned t = 0; t < 8; ++t) if (pick(3) == 0) {
            high |= (1 + pick(3)) << (t * 2);
            descriptors[t] = pick(2) | (pick(5) << 1) | (pick(32) << 4);
        }
        a |= descriptors[0] << 21;
        b = descriptors[1] | (descriptors[2] << 9) | (descriptors[3] << 18) | ((descriptors[4] & 15u) << 27);
        c = (descriptors[4] >> 4) | (descriptors[5] << 5) | (descriptors[6] << 14) | (descriptors[7] << 23);
        f.cp(0x50, low); f.cp(0x60, high); f.cp(0x70, a); f.cp(0x80, b); f.cp(0x90, c);
        f.xf(0x1018, random() & 0x3fffffff); f.xf(0x1019, random() & 0xffffff);
        f.memory.array_views = pick(4) != 0;
        for (unsigned array = 0; array < 12; ++array) {
            const std::uint32_t base = 0x1000 + array * 0x10000;
            const unsigned stride = 4 + pick(8) * 4;
            f.cp(static_cast<std::uint8_t>(0xa0 + array), base);
            f.cp(static_cast<std::uint8_t>(0xb0 + array), stride);
            std::vector<std::uint8_t> bytes(stride * 300 + 64);
            for (auto& byte : bytes) byte = static_cast<std::uint8_t>(random());
            f.memory.regions[base] = std::move(bytes);
            const unsigned units[] = {0, 0, 2, 4};
            f.memory.native_units[base] = units[pick(4)];
        }
        Draw draw; draw.vat = 0;
        std::string error;
        if (!f.decoder.state().vertex_layout(0, draw.layout, error)) continue;
        const std::uint8_t primitives[] = {0x80, 0x90, 0x98, 0xa0};
        draw.primitive = primitives[pick(4)];
        const unsigned unit = draw.primitive == 0x80 ? 4 : (draw.primitive == 0x90 ? 3 : 1);
        draw.vertex_count = static_cast<std::uint16_t>(draw.primitive >= 0x98 ? 3 + pick(40) : unit * (1 + pick(12)));
        std::vector<std::uint8_t> fifo(draw.layout.stride * draw.vertex_count);
        for (auto& byte : fifo) byte = static_cast<std::uint8_t>(random());
        // Keep indices inside the arrays except for occasional sentinels and misses.
        for (std::size_t v = 0; v < draw.vertex_count; ++v) {
            auto* p = fifo.data() + v * draw.layout.stride;
            for (bool m : draw.layout.matrix_indices) p += m;
            auto clamp_index = [&](const Attribute& attribute) {
                if (attribute.mode == AttributeMode::index8) for (unsigned i = 0; i < attribute.indices; ++i) { p[0] = static_cast<std::uint8_t>(p[0] % 250); ++p; }
                else if (attribute.mode == AttributeMode::index16) for (unsigned i = 0; i < attribute.indices; ++i) { p[0] = 0; p[1] = static_cast<std::uint8_t>(p[1] % 250); p += 2; }
                else p += attribute.fifo_bytes;
            };
            clamp_index(draw.layout.position); clamp_index(draw.layout.normal);
            for (const auto& color : draw.layout.colors) clamp_index(color);
            for (const auto& tex : draw.layout.texcoords) clamp_index(tex);
        }
        if (pick(10) == 0 && draw.layout.position.mode != AttributeMode::direct) {
            const std::size_t first = draw.primitive >= 0x98 ? 0 : unit * pick(draw.vertex_count / unit);
            const std::size_t count = draw.primitive >= 0x98 ? draw.vertex_count : unit;
            for (std::size_t v = first; v < first + count; ++v) {
                auto* p = fifo.data() + v * draw.layout.stride;
                for (bool m : draw.layout.matrix_indices) p += m;
                p[0] = 255; if (draw.layout.position.mode == AttributeMode::index16) p[1] = 255;
            }
        }
        draw.vertices = {fifo.data(), fifo.size()};
        compare(f, draw, pick(1000), "trial " + std::to_string(trial));
        ++compared;
    }
    require(compared > 3000, "too few randomized layouts");
}

// Arrays native code wrote (Aurora's little-endian arrays) decode like their big-endian
// copies in both vertex paths: numbers in host order at their own width, bytes as stored.
void host_order_arrays() {
    const float positions[4][3] = {{1.5f, -2.0f, 3.25f}, {4, 5, 6}, {-7, 8.5f, 9}, {10, 11, -12.75f}};
    const std::uint8_t colors[4][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}, {13, 14, 15, 16}};
    for (const bool host : {false, true}) {
        Fixture f;
        f.cp(0x50, (2u << 9) | (2u << 13));         // Position and colour 0 by 8-bit index.
        f.cp(0x70, 1u | (4u << 1) | (1u << 13) | (5u << 14)); // XYZ f32, RGBA8.
        f.cp(0xa0, 0x1000); f.cp(0xb0, 12); f.cp(0xa2, 0x2000); f.cp(0xb2, 4);
        std::vector<std::uint8_t> position_bytes(sizeof positions), color_bytes(sizeof colors);
        for (unsigned i = 0; i < 12; ++i) {
            std::uint32_t word; std::memcpy(&word, &positions[i / 3][i % 3], 4);
            if (host) std::memcpy(position_bytes.data() + i * 4, &word, 4);
            else for (unsigned b = 0; b < 4; ++b) position_bytes[i * 4 + b] = static_cast<std::uint8_t>(word >> (24 - 8 * b));
        }
        std::memcpy(color_bytes.data(), colors, sizeof colors);
        f.memory.regions[0x1000] = position_bytes; f.memory.regions[0x2000] = color_bytes;
        f.memory.host_order[0x1000] = host; f.memory.host_order[0x2000] = host;
        Draw draw; draw.vat = 0; draw.primitive = 0x80; draw.vertex_count = 4;
        std::string error;
        require(f.decoder.state().vertex_layout(0, draw.layout, error), "host-order layout");
        const std::uint8_t fifo[] = {0, 0, 1, 1, 2, 2, 3, 3};
        draw.vertices = {fifo, sizeof fifo};
        const auto captured = capture_vertices(f.decoder.state(), draw, f.memory);
        require(bool(captured), "host-order capture: " + captured.message);
        for (unsigned v = 0; v < 4; ++v) {
            const auto& vertex = captured.snapshot->vertices()[v];
            for (unsigned c = 0; c < 3; ++c) require(vertex.position[c] == positions[v][c], "host-order position");
            for (unsigned c = 0; c < 4; ++c) require(vertex.colors[0][c] == colors[v][c], "host-order colour bytes");
        }
        compare(f, draw, 0, host ? "host-order arrays" : "canonical arrays");
    }
}

// A merged draw converts to the records and indices of its runs converted one by one.
void merged_runs() {
    Fixture f;
    // Indexed16 XYZ float positions, direct RGBA8 color.
    f.cp(0x50, 0x2600); f.cp(0x70, 0x16009);
    f.cp(0xa0, 0x100); f.cp(0xb0, 12);
    std::vector<std::uint8_t> positions;
    for (unsigned i = 0; i < 12 * 3; ++i) {
        const float value = static_cast<float>(i) * 0.5f - 3.0f;
        std::uint32_t word; std::memcpy(&word, &value, 4);
        for (int b = 3; b >= 0; --b) positions.push_back(static_cast<std::uint8_t>(word >> (8 * b)));
    }
    f.memory.regions[0x100] = positions;
    VertexLayout layout; std::string error;
    require(f.decoder.state().vertex_layout(0, layout, error) && layout.stride == 6, "layout: " + error);
    auto vertex = [](std::vector<std::uint8_t>& out, unsigned index, std::uint8_t shade) {
        out.insert(out.end(), {0, static_cast<std::uint8_t>(index), shade, shade, shade, 255});
    };
    std::vector<std::uint8_t> payload;
    for (unsigned i = 0; i < 4; ++i) vertex(payload, i, static_cast<std::uint8_t>(10 * i));
    payload.insert(payload.end(), {0xa0, 0, 5});
    for (unsigned i = 0; i < 5; ++i) vertex(payload, 4 + i, static_cast<std::uint8_t>(7 * i));
    payload.insert(payload.end(), {0x90, 0, 3});
    for (unsigned i = 0; i < 3; ++i) vertex(payload, 9 + i, 99);
    const PrimitiveRun runs[] = {{0x98, 4, 0}, {0xa0, 5, 27}, {0x90, 3, 60}};
    Draw merged;
    merged.primitive = 0x98; merged.vat = 0; merged.vertex_count = 4; merged.layout = layout;
    merged.vertices = {payload.data(), payload.size()};
    merged.runs = runs; merged.run_count = 3;
    const auto format = stream_format(layout);
    std::vector<std::uint8_t> vertices, expected_vertices;
    std::vector<std::uint32_t> indices, expected_indices;
    const auto result = append_stream_vertices(f.decoder.state(), merged, f.memory, format, vertices, indices, 5);
    require(bool(result) && result.vertices == 12 && result.indices == 6 + 9 + 3, std::string("merged conversion: ") + result.message);
    std::uint32_t first = 5;
    for (const auto& run : runs) {
        Draw single = merged;
        single.runs = nullptr; single.run_count = 0;
        single.primitive = run.primitive; single.vertex_count = run.vertex_count;
        single.vertices = {payload.data() + run.offset, std::size_t{run.vertex_count} * layout.stride};
        const auto part = append_stream_vertices(f.decoder.state(), single, f.memory, format, expected_vertices, expected_indices, first);
        require(bool(part), std::string("run conversion: ") + part.message);
        first += part.vertices;
    }
    require(vertices == expected_vertices && indices == expected_indices, "merged records and indices equal the runs'");
    std::vector<std::uint8_t> records;
    require(!append_stream_records(f.decoder.state(), merged, f.memory, format, records), "line and point records refuse merged draws");
    require(!capture_vertices(f.decoder.state(), merged, f.memory), "packet capture refuses merged draws");
}

void fraction_scales() {
    for (unsigned fraction = 0; fraction < 32; ++fraction)
        require(fraction_scale(fraction) == std::ldexp(1.0f, -static_cast<int>(fraction)), "fraction scale " + std::to_string(fraction));
}

void partial_disable() {
    Fixture f;
    f.cp(0x50, 2u << 9); f.cp(0x70, 1 | (4u << 1));
    f.cp(0xa0, 0x1000); f.cp(0xb0, 12);
    f.memory.regions[0x1000] = std::vector<std::uint8_t>(64);
    Draw draw; draw.primitive = 0x90; draw.vertex_count = 3;
    std::string error;
    require(f.decoder.state().vertex_layout(0, draw.layout, error), error);
    std::vector<std::uint8_t> fifo{0, 255, 1};
    draw.vertices = {fifo.data(), fifo.size()};
    compare(f, draw, 0, "partial disable");
}
}

int main() {
    try {
        host_order_arrays();
        fraction_scales();
        randomized();
        partial_disable();
        merged_runs();
    } catch (const std::exception& error) {
        std::cerr << "gx_vertex_stream_tests: " << error.what() << '\n';
        return 1;
    }
    std::cout << "gx_vertex_stream_tests passed\n";
}
