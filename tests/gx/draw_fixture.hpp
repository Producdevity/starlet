#pragma once

#include "starlet/gx/draw.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace starlet::gx::test {
inline void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
inline void append(std::vector<std::uint8_t>& bytes, std::uint32_t word) {
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(word >> shift));
}
inline void append_float(std::vector<std::uint8_t>& bytes, float value) {
    std::uint32_t word; std::memcpy(&word, &value, 4); append(bytes, word);
}
struct Memory : MemoryReader {
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(32, 0xa5);
    unsigned reads = 0;
    bool fail = false;
    bool copy(std::uint32_t address, std::uint8_t* output, std::size_t size) override {
        ++reads;
        if (fail || address != 0x8000 || size > bytes.size()) return false;
        std::copy_n(bytes.data(), size, output); return true;
    }
};
struct Sink : CommandSink {
    bool draw(const State&, const Draw&, MemoryReader&) override { return true; }
    bool bp_write(const State&, std::uint8_t, std::uint32_t) override { return true; }
    bool invalidate_vertices() override { return true; }
};
struct Fixture {
    Memory memory;
    Sink sink;
    Decoder decoder{memory, sink};
    std::vector<std::uint8_t> vertices;
    void bp(unsigned reg, std::uint32_t value) {
        check(bool(decoder.write_u8(0x61)), "BP opcode");
        check(bool(decoder.write_u32((reg << 24) | value)), "BP payload");
    }
    void cp(unsigned reg, std::uint32_t value) {
        check(bool(decoder.write_u8(8)), "CP opcode");
        check(bool(decoder.write_u8(reg)), "CP address");
        check(bool(decoder.write_u32(value)), "CP payload");
    }
    void xf(unsigned reg, std::uint32_t value) {
        check(bool(decoder.write_u8(0x10)), "XF opcode");
        check(bool(decoder.write_u32(reg)), "XF address");
        check(bool(decoder.write_u32(value)), "XF payload");
    }
    void f(unsigned reg, float value) { std::uint32_t w; std::memcpy(&w, &value, 4); xf(reg, w); }
    Fixture() {
        cp(0x50, (1u << 9) | (1u << 13)); cp(0x60, 5);
        cp(0x70, 1u | (4u << 1) | (1u << 13) | (5u << 14) | (1u << 21) | (4u << 22));
        cp(0x80, 1u | (4u << 1));
        for (unsigned row : {0u, 60u}) for (unsigned y = 0; y < 3; ++y)
            for (unsigned x = 0; x < 4; ++x) f((row + y) * 4 + x, x == y ? 1 : 0);
        xf(0x1018, (60u << 6) | (60u << 12));
        xf(0x1009, 1); xf(0x100e, 1); xf(0x1010, 1);
        xf(0x103f, 2); xf(0x1040, 5u << 7); xf(0x1041, 6u << 7);
        const float projection[] = {2.0f / 640, -1, -2.0f / 456, 1, 1, 0};
        for (unsigned i = 0; i < 6; ++i) f(0x1020 + i, projection[i]);
        xf(0x1026, 1);
        const float viewport[] = {320, -228, 16777216, 662, 570, 16777216};
        for (unsigned i = 0; i < 6; ++i) f(0x101a + i, viewport[i]);
        bp(0x20, 0x156156); bp(0x21, 0x3d531d); bp(0x59, 0x02acab);
        bp(0x00, 0x12); bp(0x40, 0x06); bp(0x41, 0xf4a9); bp(0x44, 3); bp(0xf3, 0x3f0000);
        bp(0xc0, 0x8f8af);
        bp(0xc1, 0x80000 | (7u << 13) | (4u << 10) | (5u << 7) | (7u << 4));
        bp(0x28, 0x4b); // Stage0 consumes coordinate1, map3, color0.
        bp(0xf6, 4); bp(0xf7, 14);
        bp(0x32, 7); bp(0x33, 3);
        bp(0x83, 0); bp(0x87, 0); bp(0x8b, 0x100c07); bp(0x97, 0x8000 >> 5);
        const float positions[4][2] = {{100,100},{300,100},{300,200},{100,200}};
        const std::uint8_t colors[4][4] = {{255,32,64,255},{32,255,64,192},{32,64,255,128},{255,192,32,64}};
        for (unsigned i = 0; i < 4; ++i) {
            append_float(vertices, positions[i][0]); append_float(vertices, positions[i][1]); append_float(vertices, -.5f);
            vertices.insert(vertices.end(), colors[i], colors[i] + 4);
            append_float(vertices, .25f); append_float(vertices, .75f);
            append_float(vertices, i == 1 || i == 2 ? 1 : 0); append_float(vertices, i >= 2 ? 1 : 0);
        }
    }
    void full_opaque_quad() {
        bp(0x41, 0xf118); bp(0x28, 0);
        bp(0xc0, 0x80000 | (15u << 12) | (15u << 8) | (15u << 4) | 10u);
        bp(0xc1, 0x80000 | (7u << 13) | (7u << 10) | (7u << 7) | (5u << 4));
        const float corners[4][2] = {{0,0},{640,0},{640,456},{0,456}};
        for (unsigned i=0;i<4;++i) {
            for(unsigned c=0;c<2;++c) {
                std::uint32_t bits;std::memcpy(&bits,&corners[i][c],4);
                for(unsigned b=0;b<4;++b)vertices[i*32+c*4+b]=bits>>(24-b*8);
            }
            std::fill_n(vertices.data()+i*32+12, 4, 255);
        }
    }
    Draw command() {
        Draw draw; draw.primitive = 0x80; draw.vertex_count = 4;
        std::string error; check(decoder.state().vertex_layout(0, draw.layout, error), error.c_str());
        draw.vertices = {vertices.data(), vertices.size()};
        return draw;
    }
    DrawCapture capture(DrawLimits limits = {}) {
        return capture_draw(decoder.state(), command(), memory, limits);
    }
};
inline const DrawPacket& packet(const DrawCapture& c) { check(bool(c), c.status.message.c_str()); return *c.packet; }
inline const GlesDrawPipeline& pipeline(const GlesDrawResult& r) { check(bool(r), r.status.message.c_str()); return r.pipeline; }
}
