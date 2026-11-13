#include "starlet/compat/gx_fifo.h"
#include "starlet/gx/decoder.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <new>
#include <stdexcept>
#include <vector>

using namespace starlet::gx;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
void ok(Status status) { require(static_cast<bool>(status), status.message.c_str()); }

struct Memory final : MemoryReader {
    std::map<std::uint32_t, std::vector<std::uint8_t>> regions;
    unsigned reads = 0;
    bool copy(std::uint32_t address, std::uint8_t* output, std::size_t size) override {
        ++reads;
        auto it = regions.upper_bound(address);
        if (it == regions.begin()) {
            return false;
        }
        --it;
        const auto offset = std::size_t{address - it->first};
        if (offset > it->second.size() || size > it->second.size() - offset) {
            return false;
        }
        std::copy_n(it->second.data() + offset, size, output);
        return true;
    }
};

struct Sink : CommandSink {
    struct CapturedDraw {
        State state;
        std::uint8_t primitive;
        std::uint8_t vat;
        std::uint16_t count;
        VertexLayout layout;
        std::vector<std::uint8_t> vertices;
        std::vector<PrimitiveRun> runs;
        std::uint32_t total;
    };
    std::vector<CapturedDraw> draws;
    std::vector<std::uint8_t> bp_addresses;
    unsigned invalidations = 0;
    bool accept = true;
    bool draw(const State& state, const Draw& command, MemoryReader&) override {
        draws.push_back({state, command.primitive, command.vat, command.vertex_count, command.layout,
                         {command.vertices.data, command.vertices.data + command.vertices.size},
                         {command.runs, command.runs + command.run_count}, command.total_vertices()});
        return accept;
    }
    bool bp_write(const State&, std::uint8_t address, std::uint32_t) override {
        bp_addresses.push_back(address);
        return accept;
    }
    bool invalidate_vertices() override { ++invalidations; return accept; }
};

void cp(Decoder& d, std::uint8_t address, std::uint32_t value) {
    ok(d.write_u8(0x08)); ok(d.write_u8(address)); ok(d.write_u32(value));
}
void bp(Decoder& d, std::uint8_t address, std::uint32_t value) {
    ok(d.write_u8(0x61)); ok(d.write_u32((std::uint32_t{address} << 24) | value));
}
void xf(Decoder& d, std::uint16_t address, std::uint32_t value) {
    ok(d.write_u8(0x10)); ok(d.write_u16(0)); ok(d.write_u16(address)); ok(d.write_u32(value));
}
void list(Decoder& d, std::uint32_t address, std::uint32_t size) {
    ok(d.write_u8(0x40)); ok(d.write_u32(address)); ok(d.write_u32(size));
}
void point(Decoder& d) {
    ok(d.write_u8(0xb8)); ok(d.write_u16(1)); ok(d.write_u8(2)); ok(d.write_u8(3));
}
void position_xy_u8(Decoder& d) { cp(d, 0x50, 0x200); cp(d, 0x70, 0); }

void bp_mask_and_order() {
    Memory memory; Sink sink; Decoder d(memory, sink);
    require(d.state().bp(0xfe) == 0xffffff, "initial BP mask");
    bp(d, 0x12, 0x123456);
    bp(d, 0xfe, 0x00ff00);
    bp(d, 0x12, 0xabcdef);
    require(d.state().bp(0x12) == 0x12cd56, "one-shot BP merge");
    require(d.state().bp(0xfe) == 0xffffff, "BP mask resets");
    bp(d, 0x12, 0x112233);
    require(d.state().bp(0x12) == 0x112233, "following BP full write");
    bp(d, 0xfe, 0);
    cp(d, 0x30, 0x89abcdef);
    xf(d, 0x1008, 0x12345678);
    bp(d, 0x12, 0xffffff);
    require(d.state().bp(0x12) == 0x112233, "CP/XF do not consume BP mask");
    require(d.state().cp(0x30) == 0x89abcdef && d.state().xf(0x1008) == 0x12345678, "CP/XF endian");
    require(sink.bp_addresses == std::vector<std::uint8_t>({0x12, 0xfe, 0x12, 0x12, 0xfe, 0x12}), "BP effect ordering");
    ok(d.finish());
}

void tev_register_banks() {
    Memory memory; Sink sink; Decoder d(memory, sink);
    bp(d, 0xe2, 0x456123); bp(d, 0xe3, 0x321654);
    bp(d, 0xe2, 0x8ab0cd); bp(d, 0xe3, 0x8ef087);
    require(d.state().tev_register(false, 2) == 0x456123 && d.state().tev_register(false, 3) == 0x321654,
            "konst writes preserve regular TEV bank");
    require(d.state().tev_register(true, 2) == 0x0ab0cd && d.state().tev_register(true, 3) == 0x0ef087,
            "konst bank retains both halves");
    require(d.state().bp(0xe2) == 0x8ab0cd, "raw BP retains last banked command");
    State captured = d.state();
    bp(d, 0xfe, 0xff); bp(d, 0xe2, 0x12);
    require(d.state().tev_register(true, 2) == 0x0ab012 && d.state().tev_register(false, 2) == 0x456123,
            "masked payload uses merged bank selector");
    bp(d, 0xfe, 0); bp(d, 0xe2, 0xffffff);
    require(d.state().tev_register(true, 2) == 0x0ab012, "zero mask preserves selected bank");
    bp(d, 0xe2, 0x123456);
    require(d.state().tev_register(false, 2) == 0x123456 && d.state().tev_register(true, 2) == 0x0ab012,
            "full bank switches update only selected bank");
    require(captured.tev_register(false, 2) == 0x456123 && captured.tev_register(true, 2) == 0x0ab0cd,
            "state capture owns both bank values");
    bool caught = false;
    try { d.state().tev_register(false, 8); } catch (const std::out_of_range&) { caught = true; }
    require(caught, "TEV half bounds");
    ok(d.finish());
    bp(d, 0xfe, 0x800000);
    const auto revision = d.state().revision();
    ok(d.write_u8(0x61));
    require(d.write_u32(0xe2800000).error == Error::unsupported_register,
            "partially masked cross-bank switch is explicit unsupported state");
    require(d.state().bp(0xe2) == 0x123456 && d.state().tev_register(false, 2) == 0x123456 &&
            d.state().tev_register(true, 2) == 0x0ab012 && d.state().revision() == revision,
            "unsupported bank switch does not mutate either bank or raw BP");
}

void texgen_fields() {
    Memory memory; Sink sink; Decoder d(memory, sink);
    xf(d, 0x1040, 0x00000282); // STQ projection, AB11 input, TEX0 source.
    auto tex = d.state().texgen(0);
    require(tex.projected && !tex.input_abc1 && tex.source_row == 5, "projection is bit one");
    xf(d, 0x1040, 0x00000004); // ST projection, ABC1 input, position source.
    tex = d.state().texgen(0);
    require(!tex.projected && tex.input_abc1 && tex.source_row == 0, "input form is bit two");
    xf(d, 0x1047, 0x0002b510);
    tex = d.state().texgen(7);
    require(tex.type == 1 && tex.source_row == 10 && tex.bump_source == 3 && tex.bump_light == 5, "bump texgen fields");
}

void layouts_and_streaming() {
    Memory memory; Sink sink; Decoder d(memory, sink);
    // PN matrix byte, XYZ float position, RGBA8 color, ST signed16 texture.
    cp(d, 0x50, 0x2201);
    cp(d, 0x60, 1);
    cp(d, 0x72, 0x18e16009);
    VertexLayout layout; std::string error;
    require(d.state().vertex_layout(2, layout, error), "VAT2 layout accepted");
    require(layout.stride == 21 && layout.position.fifo_bytes == 12 && layout.colors[0].fifo_bytes == 4 &&
            layout.texcoords[0].fifo_bytes == 4 && layout.texcoords[0].fraction_bits == 12, "explicit packed vertex byte count");
    ok(d.write_u8(0xba)); ok(d.write_u16(1));
    ok(d.write_u8(6)); ok(d.write_f32(1.0f)); ok(d.write_f32(-2.0f)); ok(d.write_f32(0.5f));
    ok(d.write_u32(0x10203040)); ok(d.write_u16(0x1234));
    require(sink.draws.empty(), "incomplete draw is not published");
    ok(d.write_u16(0xfffe));
    require(sink.draws.size() == 1 && sink.draws[0].primitive == 0xb8 && sink.draws[0].vat == 2, "draw opcode and VAT");
    const std::vector<std::uint8_t> expected = {6, 0x3f,0x80,0,0, 0xc0,0,0,0, 0x3f,0,0,0, 0x10,0x20,0x30,0x40, 0x12,0x34,0xff,0xfe};
    require(sink.draws[0].vertices == expected, "typed FIFO writes encode big endian floats and integers");
    {
        Sink other; Decoder e(memory, other);
        cp(e, 0x50, 0x2201); cp(e, 0x60, 1); cp(e, 0x72, 0x18e16009);
        const std::uint8_t header[] = {0xba, 0, 1};
        require(!e.extend_pending({header, 1}), "nothing buffered yet");
        ok(e.feed({header, sizeof(header)}));
        for (std::size_t i = 0; i + 1 < expected.size(); ++i)
            require(e.extend_pending({expected.data() + i, 1}), "payload byte extends the buffered draw");
        require(!e.extend_pending({expected.data() + expected.size() - 1, 1}) && other.draws.empty(), "completing byte is left to feed");
        ok(e.feed({expected.data() + expected.size() - 1, 1}));
        require(other.draws.size() == 1 && other.draws[0].vertices == expected, "extended payload publishes the same draw");
    }
    // XY indexed16 position + three indexed8 NBT vectors.
    cp(d, 0x50, 0x1600); cp(d, 0x60, 0); cp(d, 0x77, 0x80001200);
    require(d.state().vertex_layout(7, layout, error) && layout.stride == 5 && layout.normal.indices == 3, "NBT3 indexed byte count");
    cp(d, 0x77, 0x00001200);
    require(d.state().vertex_layout(7, layout, error) && layout.stride == 3, "NBT single index byte count");
    cp(d, 0x50, 0x0a00);
    require(d.state().vertex_layout(7, layout, error) && layout.stride == 38, "direct NBT has nine components");
    // TEX4 component/type is in VAT B; its fraction is in VAT C.
    cp(d, 0x50, 0x200); cp(d, 0x60, 0x100); cp(d, 0x70, 0); cp(d, 0x80, 0x38000000); cp(d, 0x90, 17);
    require(d.state().vertex_layout(0, layout, error) && layout.stride == 6 && layout.texcoords[4].fraction_bits == 17,
            "TEX4 VAT word boundary");
    cp(d, 0x50, 0xa00); cp(d, 0x60, 0); cp(d, 0x70, 0x400);
    require(d.state().vertex_layout(0, layout, error) && layout.normal.fraction_bits == 6, "S8 normal implied scale");
    cp(d, 0x70, 0xc00);
    require(d.state().vertex_layout(0, layout, error) && layout.normal.fraction_bits == 14, "S16 normal implied scale");
    ok(d.write_u8(0x48)); require(sink.invalidations == 1, "vertex cache invalidation delivered");
    ok(d.finish());
}

void layout_revision_ignores_arrays() {
    Memory memory; Sink sink; Decoder d(memory, sink);
    position_xy_u8(d);
    point(d);
    const auto layouts = d.state().cp_layout_revision(), all = d.state().cp_revision();
    cp(d, 0xa0, 0x1000); cp(d, 0xb0, 12);
    require(d.state().cp_layout_revision() == layouts && d.state().cp_revision() == all + 2, "array bindings leave the layout revision");
    point(d);
    cp(d, 0x70, 1);
    require(d.state().cp_layout_revision() == layouts + 1, "VAT writes advance the layout revision");
    ok(d.write_u8(0xb8)); ok(d.write_u16(1)); ok(d.write_u8(1)); ok(d.write_u8(2)); ok(d.write_u8(3));
    require(sink.draws.size() == 3 && sink.draws[1].layout.stride == 2 && sink.draws[2].layout.stride == 3 &&
            sink.draws[2].layout.position.components == 3, "draws decode with the current VAT layout");
}

// A reader that keeps matrix arrays in host order converts them in copy_indexed_xf.
void indexed_xf_reader_hook() {
    struct HostMatrices final : MemoryReader {
        Memory canonical;
        std::uint8_t last_array = 0;
        unsigned indexed = 0;
        bool copy(std::uint32_t address, std::uint8_t* output, std::size_t size) override {
            return canonical.copy(address, output, size);
        }
        bool copy_indexed_xf(std::uint8_t array, std::uint32_t address, std::uint8_t* output, std::size_t size) override {
            ++indexed;
            last_array = array;
            if (!canonical.copy(address, output, size)) return false;
            for (std::size_t i = 0; i + 4 <= size; i += 4) std::reverse(output + i, output + i + 4);
            return true;
        }
    } memory;
    Sink sink; Decoder d(memory, sink);
    memory.canonical.regions[0x2000] = {0, 0, 0x80, 0x3f, 0, 0, 0, 0xc0};  // 1.0f, -2.0f stored little-endian
    cp(d, 0xac, 0x2000); cp(d, 0xbc, 8);
    const std::uint8_t indexed[] = {0x20, 0,0, 0x10,0x40};
    ok(d.feed({indexed, sizeof(indexed)}));
    require(memory.indexed == 1 && memory.last_array == 12, "indexed XF loads read through copy_indexed_xf with their array");
    require(d.state().xf(0x40) == 0x3f800000 && d.state().xf(0x41) == 0xc0000000, "copy_indexed_xf returns canonical words");
    const std::uint8_t vertex[] = {0x61, 0x12, 0x12, 0x34, 0x56};
    ok(d.feed({vertex, sizeof(vertex)}));
    require(memory.indexed == 1, "other reads do not use copy_indexed_xf");
}

void indexed_xf_and_bounds() {
    Memory memory; Sink sink; Decoder d(memory, sink);
    memory.regions[0x1000] = {0,0,0,0, 0,0,0,0, 0x3f,0x80,0,0, 0xc0,0,0,0};
    cp(d, 0xae, 0x1000); cp(d, 0xbe, 8);
    const std::uint8_t indexed[] = {0x30, 0,1, 0x10,0x20};
    ok(d.feed({indexed, sizeof(indexed)}));
    require(d.state().xf(0x20) == 0x3f800000 && d.state().xf(0x21) == 0xc0000000, "indexed XF uses array C, stride, index and BE words");
    auto write = d.state().last_xf_memory_write();
    require(write.revision == d.state().xf_memory_revision() && write.first == 0x20 && write.end == 0x22, "indexed XF write range");
    xf(d, 0x1008, 1);
    require(d.state().last_xf_memory_write().revision == write.revision, "register writes leave the memory write");
    xf(d, 0x0c, 7);
    write = d.state().last_xf_memory_write();
    require(write.revision == d.state().xf_memory_revision() && write.first == 0x0c && write.end == 0x0d, "XF memory write range");
    const std::uint8_t last_xf[] = {0x10, 0,1, 0x10,0x57, 0,0,0,1, 0,0,0,2};
    require(d.feed({last_xf, sizeof(last_xf)}).error == Error::invalid_xf_range, "XF overrun rejected");
    require(d.state().xf(0x1057) == 0, "invalid XF range has no partial writes");
    require(d.write_u8(0).error == Error::invalid_xf_range, "failure remains latched");
}

void palettes_snapshot_and_reload() {
    Memory memory; Sink sink; Decoder d(memory, sink); position_xy_u8(d);
    memory.regions[0x2000] = std::vector<std::uint8_t>(512, 0x11);
    bp(d, 0x64, 0x100); bp(d, 0x65, 0x4001); // 256 entries at upper TMEM + 512.
    point(d);
    std::uint64_t before = 0, after = 0;
    require(d.state().palette_generation(512, 512, before) && before > 0, "first palette generation");
    memory.regions[0x2000].assign(512, 0x77);
    bp(d, 0x65, 0x4001);
    point(d);
    require(d.state().palette_generation(512, 512, after) && after > before, "same-frame identical load command advances generation");
    std::vector<std::uint8_t> bytes(512);
    require(sink.draws[0].state.read_palette(512, bytes.data(), bytes.size()) && bytes.front() == 0x11 && bytes.back() == 0x11,
            "old draw retains old palette bytes");
    require(sink.draws[1].state.read_palette(512, bytes.data(), bytes.size()) && bytes.front() == 0x77, "new draw sees new palette bytes");
    bp(d, 0x65, 0x0402); // Sixteen entries, only first 32 bytes initialized in this block.
    require(d.state().read_palette(1024, bytes.data(), 32), "short palette is available");
    require(!d.state().read_palette(1056, bytes.data(), 32), "unloaded TLUT bytes are not manufactured");
    require(!d.state().palette_generation(1056, 32, after), "unloaded palette cannot form cache key");
    memory.regions[0x2000].assign(512, 0x99);
    bp(d, 0x65, 0x0401);
    require(d.state().read_palette(512, bytes.data(), 512) && bytes[0] == 0x99 && bytes[31] == 0x99 && bytes[32] == 0x77,
            "short reload preserves untouched palette entries");
    memory.regions[0x2000].assign(512, 0x5a);
    bp(d, 0x64, 0xe00100); // Bits 21-23 set: not part of the source address.
    bp(d, 0x65, 0x0401);
    require(d.state().read_palette(512, bytes.data(), 32) && bytes[0] == 0x5a && bytes[31] == 0x5a,
            "TLUT source ignores register bits 21-23");
}

void lists_mutation_and_shared_state() {
    Memory memory; Sink sink; Decoder d(memory, sink);
    memory.regions[0x3000] = {0x08,0x50,0,0,2,0, 0x08,0x70,0,0,0,0,
                            0x61,0xfe,0,0,0xff, 0x61,0x12,0,0,0x34, 0xb8,0,1,9,8};
    bp(d, 0x12, 0x123400);
    list(d, 0x3000, 27);
    require(d.state().bp(0x12) == 0x123434 && sink.draws.size() == 1, "DL and direct path share BP mask and vertex state");
    memory.regions[0x3000][21] = 0x56;
    memory.regions[0x3000][25] = 7;
    list(d, 0x3000, 27);
    require(d.state().bp(0x12) == 0x123456 && sink.draws[1].vertices[0] == 7 && sink.draws[0].vertices[0] == 9,
            "mutable DL is freshly read on each invocation");
    point(d);
    require(sink.draws.size() == 3 && sink.draws.back().vertices[0] == 2, "DL state remains current for direct draw");
    require(memory.reads == 2, "each display list owns one memory snapshot");
    ok(d.finish());
}

void rejected_inputs_and_limits() {
    Memory memory; Sink sink;
    { Decoder d(memory, sink); const std::uint8_t x[] = {0x08,0x78,0,0,0,0};
      require(d.feed({x,sizeof(x)}).error == Error::unsupported_register, "invalid VAT register rejected"); }
    { Decoder d(memory, sink); require(d.write_u8(0x7f).error == Error::unknown_opcode, "unknown opcode rejected"); }
    { Decoder d(memory, sink); ok(d.write_u8(0x61)); require(d.finish().error == Error::truncated_command, "incomplete FIFO rejected at finish"); }
    { Decoder d(memory, sink); require(d.feed({nullptr,1}).error == Error::invalid_input, "null bytes rejected"); }
    { Decoder d(memory, sink); const std::uint8_t x[] = {0x10,0,16,0,0};
      require(d.feed({x,sizeof(x)}).error == Error::invalid_input, "oversized XF packet rejected before allocation"); }
    { Decoder d(memory, sink); cp(d,0x50,0x200); cp(d,0x70,0x0e);
      const std::uint8_t x[] = {0xb8,0,1}; require(d.feed({x,sizeof(x)}).error == Error::invalid_vertex_layout, "invalid position format rejected"); }
    { Limits limits; limits.max_display_list_depth=2; Decoder d(memory,sink,limits);
      memory.regions[0x4000] = {0x40,0,0,0x40,0,0,0,0,9};
      const auto& x=memory.regions[0x4000]; require(d.feed({x.data(),x.size()}).error == Error::display_list_depth, "recursive DL terminates"); }
    { Limits limits; limits.max_commands=3; Decoder d(memory,sink,limits);
      const std::uint8_t x[]={0,0,0,0}; require(d.feed({x,sizeof(x)}).error == Error::work_limit, "command budget"); }
    { Limits limits; limits.max_commands=2; Decoder d(memory,sink,limits);
      const std::uint8_t x[]={8,0xa0,0,0,0,1, 8,0xa1,0,0,0,2, 8,0xa2,0,0,0,3};
      require(d.feed({x,sizeof(x)}).error == Error::work_limit && d.state().cp(0xa0)==1 &&
              d.state().cp(0xa1)==2 && d.state().cp(0xa2)==0, "batched CP honors the command budget before applying a word"); }
    { Limits limits; limits.max_command_bytes=6; limits.max_expanded_bytes=12; Decoder d(memory,sink,limits);
      const std::uint8_t x[]={8,0xa0,0,0,0,1, 8,0xa1,0,0,0,2, 8,0xa2,0,0,0,3};
      require(d.feed({x,sizeof(x)}).error == Error::work_limit && d.state().cp(0xa1)==2 && d.state().cp(0xa2)==0,
              "batched CP honors the expanded byte budget"); }
    { Limits limits; limits.max_command_bytes=16; Decoder d(memory,sink,limits); cp(d,0x50,0x200); cp(d,0x70,9);
      const std::uint8_t x[]={0x90,0,2}; require(d.feed({x,sizeof(x)}).error == Error::command_size, "draw allocation limit"); }
    { Decoder d(memory,sink); memory.regions[0x5000]={0x61};
      const std::uint8_t x[]={0x40,0,0,0x50,0,0,0,0,1};
      require(d.feed({x,sizeof(x)}).error == Error::truncated_command, "truncated DL is not continued by outer FIFO"); }
    { Sink rejecting; rejecting.accept=false; Decoder d(memory,rejecting);
      require(d.write_u8(0x48).error == Error::sink_rejected, "backend rejection is explicit"); }
}


void failed_memory_is_not_applied() {
    Memory memory; Sink sink;
    { Decoder d(memory,sink); cp(d,0xac,0xfffffff0); cp(d,0xbc,255);
      const std::uint8_t x[]={0x20,0xff,0xff,0,0};
      require(d.feed({x,sizeof(x)}).error==Error::memory_read && d.state().xf(0)==0, "indexed XF address overflow rejected atomically"); }
    { Decoder d(memory,sink); cp(d,0xad,0x7000); cp(d,0xbd,4);
      const std::uint8_t x[]={0x28,0,0,0,0};
      require(d.feed({x,sizeof(x)}).error==Error::memory_read && d.state().xf(0)==0, "missing indexed XF memory rejected"); }
    { Decoder d(memory,sink); bp(d,0x64,0x380);
      const std::uint8_t x[]={0x61,0x65,0,4,0};
      require(d.feed({x,sizeof(x)}).error==Error::memory_read && d.state().bp(0x65)==0, "missing TLUT source does not publish load"); }
    { Decoder d(memory,sink); bp(d,0x64,0x380);
      const std::uint8_t x[]={0x61,0x65,0,0x47,0xff};
      require(d.feed({x,sizeof(x)}).error==Error::invalid_input && d.state().bp(0x65)==0, "TLUT destination overflow rejected before source read"); }
    { Limits limits; limits.max_command_bytes=64; limits.max_expanded_bytes=64; Decoder d(memory,sink,limits);
      const std::uint8_t x[]={0x40,0,0,0x70,0,0,0,0,60};
      require(d.feed({x,sizeof(x)}).error==Error::work_limit, "DL bytes bounded before allocation or read"); }
    { Limits limits; limits.max_display_list_depth=65; Decoder d(memory,sink,limits);
      require(d.status().error==Error::invalid_input, "host stack depth has a fixed upper bound"); }
}

void immutable_list_during_execution() {
    Memory memory;
    struct MutatingSink final : Sink {
        explicit MutatingSink(Memory& m) : memory(m) {}
        Memory& memory;
        bool bp_write(const State& state, std::uint8_t address, std::uint32_t value) override {
            memory.regions[0x6000][9]=0x99;
            return Sink::bp_write(state,address,value);
        }
    } sink(memory);
    Decoder d(memory,sink);
    memory.regions[0x6000]={0x61,0x11,0,0,1, 0x61,0x12,0,0,2};
    list(d,0x6000,10);
    require(d.state().bp(0x12)==2, "mutation during a DL cannot alter its captured bytes");
    list(d,0x6000,10);
    require(d.state().bp(0x12)==0x99, "next DL invocation observes CPU mutation");
}

void callback_reentry_is_rejected() {
    Memory memory;
    struct ReentrantSink final : Sink {
        Decoder* decoder=nullptr;
        bool invalidate_vertices() override {
            require(decoder->write_u8(0).error==Error::reentrant_call, "callback reentry reports error");
            return true;
        }
    } sink;
    Decoder d(memory,sink); sink.decoder=&d;
    const std::uint8_t bytes[]={0x48,0x61,0x12,0,0,1};
    require(d.feed({bytes,sizeof(bytes)}).error==Error::reentrant_call && d.state().bp(0x12)==0,
            "reentrant callback halts later commands");
}

void processing_exceptions_latch() {
    Memory memory;
    {
        struct ThrowingSink final : Sink {
            unsigned writes = 0;
            bool bp_write(const State&, std::uint8_t, std::uint32_t) override {
                ++writes;
                throw std::bad_alloc();
            }
        } sink;
        Decoder d(memory, sink);
        ok(d.write_u8(0x61));
        bool caught = false;
        try {
            d.write_u32(0x12000001);
        } catch (const std::bad_alloc&) {
            caught = true;
        }
        require(caught, "allocation exception type is preserved");
        require(d.status().error == Error::processing_exception && d.status().message.empty(),
                "allocation exception latches without an error-message allocation");
        require(d.state().bp(0x12) == 1 && d.state().revision() == 1, "failed callback state remains inspectable");
        require(d.write_u8(0).error == Error::processing_exception && d.finish().error == Error::processing_exception,
                "feed and finish retain exception failure");
        require(sink.writes == 1 && d.state().revision() == 1, "buffered BP is not replayed after an exception");
    }
    {
        struct Failure { int code; };
        struct ThrowingMemory final : MemoryReader {
            unsigned reads = 0;
            bool copy(std::uint32_t, std::uint8_t*, std::size_t) override {
                ++reads;
                throw Failure{42};
            }
        } throwing_memory;
        Sink sink;
        Decoder d(throwing_memory, sink);
        bp(d, 0x64, 0x100);
        bool caught = false;
        try {
            bp(d, 0x65, 0x400);
        } catch (const Failure& failure) {
            caught = failure.code == 42;
        }
        require(caught, "non-standard memory callback exception is preserved");
        require(d.status().error == Error::processing_exception && d.state().bp(0x65) == 0,
                "throwing palette read does not publish the load");
        require(d.write_u8(0).error == Error::processing_exception && throwing_memory.reads == 1,
                "throwing memory command is not replayed");
    }
    {
        struct ThrowingSink final : Sink {
            unsigned draw_calls = 0;
            bool draw(const State&, const Draw&, MemoryReader&) override {
                ++draw_calls;
                throw std::runtime_error("snapshot failure");
            }
        } sink;
        Decoder d(memory, sink);
        position_xy_u8(d);
        bool caught = false;
        try {
            point(d);
        } catch (const std::runtime_error&) {
            caught = true;
        }
        require(caught && d.status().error == Error::processing_exception, "draw exception is latched and propagated");
        require(d.write_u8(0).error == Error::processing_exception && sink.draw_calls == 1,
                "buffered draw is not replayed");
    }
}

void merged_primitives() {
    // XY u8 direct positions (stride 2) on VAT 0 and VAT 1.
    const std::vector<std::uint8_t> bytes = {0x08,0x50,0,0,2,0, 0x08,0x70,0,0,0,0, 0x08,0x71,0,0,0,0,
        0x98,0,3, 1,1,2,2,3,3,   0x98,0,4, 4,4,5,5,6,6,7,7,   0xa0,0,3, 8,8,9,9,10,10,
        0x08,0x30,0,0,0,0,       0x98,0,3, 1,2,3,4,5,6,       0xa8,0,2, 1,1,2,2,
        0x99,0,3, 1,1,2,2,3,3,   0x80,0,4, 0,0,1,1,2,2,3,3};
    Memory memory; Sink sink; Limits limits; limits.merge_primitives = true;
    Decoder d(memory, sink, limits);
    ok(d.feed({bytes.data(), bytes.size()})); ok(d.finish());
    require(sink.draws.size() == 5, "only back-to-back triangle commands of one VAT merge");
    const auto& first = sink.draws[0];
    require(first.runs.size() == 3 && first.total == 10 && first.primitive == 0x98 && first.count == 3 &&
            first.runs[1].primitive == 0x98 && first.runs[1].vertex_count == 4 && first.runs[1].offset == 9 &&
            first.runs[2].primitive == 0xa0 && first.runs[2].offset == 20 && first.vertices.size() == 26 &&
            first.vertices[first.runs[2].offset] == 8, "merged runs name each command's vertices");
    require(sink.draws[1].runs.empty() && sink.draws[1].total == 3, "a draw followed by a line stays single");
    require(sink.draws[2].primitive == 0xa8 && sink.draws[2].runs.empty(), "lines never merge");
    require(sink.draws[3].vat == 1 && sink.draws[3].runs.empty() && sink.draws[4].vat == 0, "another VAT ends a merge");

    Sink plain; Decoder e(memory, plain);
    ok(e.feed({bytes.data(), bytes.size()}));
    require(plain.draws.size() == 7 && plain.draws[0].runs.empty(), "merging is opt-in");

    // PN matrix byte + XY u8: the last run's last vertex latches the matrix index.
    const std::vector<std::uint8_t> latched = {0x08,0x50,0,0,2,1, 0x08,0x70,0,0,0,0,
        0x98,0,3, 3,1,1, 3,2,2, 3,3,3,   0x98,0,3, 3,1,1, 3,2,2, 9,3,3};
    Sink matrices; Decoder f(memory, matrices, limits);
    ok(f.feed({latched.data(), latched.size()}));
    require(matrices.draws.size() == 1 && matrices.draws[0].runs.size() == 2 && f.state().matrix_index(0) == 9,
            "a merged draw latches the matrix index of its last vertex");
}

void host_commands_and_filtered_bp() {
    // Opcode 0x50 carries a length byte and the commands it stands for; BP 0x12 writes are
    // dropped, BP 0x13 writes gain bit 8 and also write BP 0x14.
    struct HostSink final : Sink {
        Decoder* decoder = nullptr;
        unsigned hosts = 0;
        bool host_command(const State&, ByteView input, std::size_t& consumed) override {
            ++hosts;
            if (input.size < 2) return false;
            consumed = 2 + input.data[1];
            return consumed > input.size || static_cast<bool>(decoder->inject({input.data + 2, input.data[1]}));
        }
        bool filter_bp(const State&, std::uint8_t address, std::uint32_t& value) override {
            if (address == 0x12) return false;
            value |= 0x100;
            const std::uint8_t write[] = {0x61, 0x14, 0, 0, 9};
            return static_cast<bool>(decoder->inject({write, sizeof write}));
        }
        bool invalidate_vertices() override {
            require(decoder->inject({nullptr, 0}).error == Error::reentrant_call, "inject outside host callbacks is reentry");
            return true;
        }
    };
    Limits limits;
    limits.host_opcodes.set(0x50);
    limits.filtered_bp.set(0x12);
    limits.filtered_bp.set(0x13);
    limits.dropped_bp.set(0x17);
    Memory memory;
    {
        HostSink sink; Decoder d(memory, sink, limits); sink.decoder = &d;
        const std::uint8_t pre[] = {0x08,0x50,0,0,2,0, 0x08,0x70,0,0,0,0};
        ok(d.inject({pre, sizeof pre}));
        const std::vector<std::uint8_t> bytes = {0x61,0x12,0,0,5, 0x61,0x13,0,0,1, 0x61,0x17,0,0,8,
            0x50,15, 0x61,0x12,0,0,7, 0x61,0x17,0,0,2, 0xb8,0,1,0xaa,0xbb, 0x61,0x15,0,0,3};
        ok(d.feed({bytes.data(), bytes.size()}));
        require(d.state().bp(0x12) == 7 && d.state().bp(0x13) == 0x101 && d.state().bp(0x14) == 9 && d.state().bp(0x15) == 3 &&
                d.state().bp(0x17) == 2, "filtered writes drop or rewrite, dropped writes vanish; injected writes skip both");
        require(sink.hosts == 1 && sink.draws.size() == 1 && sink.draws[0].vertices == std::vector<std::uint8_t>({0xaa,0xbb}),
                "a host command applies the commands it injects in place");
        const std::uint8_t invalidate[] = {0x48};
        require(d.feed({invalidate, 1}).error == Error::reentrant_call, "draw-side callbacks cannot inject");
    }
    {
        // A host list call injects a real CALL_DL; the list's own commands pass the filter and
        // reach host_command again.
        Memory lists;
        lists.regions[0x8000] = {0x61,0x13,0,0,2, 0x50,5, 0x61,0x12,0,0,4, 0x61,0x16,0,0,6};
        HostSink sink; Decoder d(lists, sink, limits); sink.decoder = &d;
        const std::uint8_t bytes[] = {0x50, 9, 0x40,0,0,0x80,0,0,0,0,17};
        ok(d.feed({bytes, sizeof bytes}));
        require(sink.hosts == 2 && d.state().bp(0x13) == 0x102 && d.state().bp(0x14) == 9 && d.state().bp(0x12) == 4 &&
                d.state().bp(0x16) == 6, "a list run by an injected call decodes as stream commands");
    }
    {
        HostSink sink; Decoder d(memory, sink, limits); sink.decoder = &d;
        const std::uint8_t bytes[] = {0x50, 9, 0x61};
        require(d.feed({bytes, sizeof bytes}).error == Error::truncated_command, "a host command must fit its feed");
    }
    {
        HostSink sink; Decoder d(memory, sink, limits); sink.decoder = &d;
        const std::uint8_t bytes[] = {0x50};
        require(d.feed({bytes, sizeof bytes}).error == Error::sink_rejected, "a refused host command stops the stream");
    }
    {
        Sink sink; Decoder d(memory, sink);
        const std::uint8_t partial[] = {0x61, 0x12}, write[] = {0x61, 0x13, 0, 0, 1};
        ok(d.feed({partial, sizeof partial}));
        require(d.inject({write, sizeof write}).error == Error::reentrant_call, "no injection inside a partial command");
    }
}

void tev_banks_in_fed_writes() {
    // Plain feed writes of E0-E7 keep both banks and leave the state revision alone for color values.
    Memory memory; Sink sink; Decoder d(memory, sink);
    const std::uint8_t konst[] = {0x61,0xe2,0x80,0x01,0x02};
    ok(d.feed({konst, sizeof konst}));
    const auto state_revision = d.state().bp_state_revision(), bp_revision = d.state().bp_revision();
    const std::uint8_t color[] = {0x61,0xe2,0x00,0x03,0x04};
    ok(d.feed({color, sizeof color}));
    require(d.state().tev_register(1, 2) == 0x000102 && d.state().tev_register(0, 2) == 0x000304 && d.state().bp(0xe2) == 0x000304,
            "fed TEV writes fill their bank");
    require(d.state().bp_state_revision() == state_revision && d.state().bp_revision() == bp_revision + 1,
            "a TEV color value changes registers but not program state");
}

void every_split_boundary() {
    const std::vector<std::uint8_t> bytes = {0x08,0x50,0,0,2,0, 0x08,0x70,0,0,0,0,
        0x10,0,1,0x10,0x40,0,0,0,2,0,0,0,4, 0x61,0x12,0x12,0x34,0x56, 0xb8,0,1,0xaa,0xbb, 0x48};
    for (std::size_t split=0; split<=bytes.size(); ++split) {
        Memory memory; Sink sink; Decoder d(memory,sink);
        ok(d.feed({bytes.data(),split})); ok(d.feed({bytes.data()+split,bytes.size()-split})); ok(d.finish());
        require(d.state().bp(0x12)==0x123456 && d.state().xf(0x1041)==4 && sink.invalidations==1 &&
                sink.draws.size()==1 && sink.draws[0].vertices==std::vector<std::uint8_t>({0xaa,0xbb}), "all FIFO split points equivalent");
    }
}
}

void list_fast_path_matches_general_path() {
    // The same commands applied by inject (always the general path) and from a display list
    // (fast_command) leave identical state, revisions and BP callbacks.
    struct Recorder final : Sink {
        std::vector<std::pair<std::uint8_t, std::uint32_t>> writes;
        bool bp_write(const State& s, std::uint8_t address, std::uint32_t value) override {
            writes.push_back({address, value});
            return Sink::bp_write(s, address, value);
        }
    };
    std::vector<std::uint8_t> commands = {
        0x61,0x28,0x00,0x00,0x4b, 0x61,0xe2,0x80,0x01,0x02, 0x61,0xe2,0x00,0x03,0x04, 0x61,0x28,0x00,0x00,0x4b,
        0x00,0x00,0x00,
        0x08,0x50,0x00,0x00,0x02,0x00, 0x08,0xa0,0x00,0x00,0x90,0x00,
        0x10,0x00,0x02,0x00,0x00, 0x3f,0x80,0,0, 0,0,0,0, 0x41,0x20,0,0,
        0x10,0x00,0x02,0x00,0x00, 0x3f,0x80,0,0, 0,0,0,0, 0x41,0x20,0,0,
        0x10,0x00,0x00,0x05,0xf4, 0x3f,0x80,0,0,
        0x61,0xe3,0x00,0x05,0x06, 0x61,0x30,0x12,0x34,0x56};
    while (commands.size() % 32) commands.push_back(0);
    Memory general_memory, list_memory;
    list_memory.regions[0x8000] = commands;
    Recorder general_sink, list_sink;
    Decoder general(general_memory, general_sink), listed(list_memory, list_sink);
    ok(general.inject({commands.data(), commands.size()}));
    const std::uint8_t call[] = {0x40, 0,0,0x80,0, 0,0,0,static_cast<std::uint8_t>(commands.size())};
    ok(listed.feed({call, sizeof call}));
    const auto& a = general.state();
    const auto& b = listed.state();
    bool same = true;
    for (unsigned r = 0; r < 256; ++r) same = same && a.bp(std::uint8_t(r)) == b.bp(std::uint8_t(r)) && a.cp(std::uint8_t(r)) == b.cp(std::uint8_t(r));
    for (std::uint16_t x = 0; x < 0x1000; ++x) same = same && a.xf(x) == b.xf(x);
    for (bool konst : {false, true}) for (std::uint8_t h = 0; h < 8; ++h) same = same && a.tev_register(konst, h) == b.tev_register(konst, h);
    require(same, "a list's plain writes leave the state the general path leaves");
    require(a.bp_revision() == b.bp_revision() && a.bp_state_revision() == b.bp_state_revision() && a.cp_revision() == b.cp_revision() &&
            a.cp_layout_revision() == b.cp_layout_revision() && a.xf_memory_revision() == b.xf_memory_revision() &&
            a.xf_register_revision() == b.xf_register_revision() &&
            a.last_xf_memory_write().first == b.last_xf_memory_write().first && a.last_xf_memory_write().end == b.last_xf_memory_write().end,
            "and the same revisions");
    require(general_sink.writes == list_sink.writes, "and the same BP callbacks in order");
    require(b.bp(0x30) == 0x123456 && b.tev_register(true, 2) == 0x000102 && b.tev_register(false, 2) == 0x000304, "the list was applied");
}

void selective_bp_notifications() {
    struct Selected : Sink {
        bool observes_bp(std::uint8_t address) const override { return address == 0x45 || address == 0x52; }
    } sink;
    Memory memory; memory.regions[0x1000] = std::vector<std::uint8_t>(32, 0x5a);
    Decoder decoder(memory, sink);
    bp(decoder, 0xc0, 0x123456); bp(decoder, 0xfe, 255); bp(decoder, 0xc0, 0xabcd88);
    bp(decoder, 0x45, 2); bp(decoder, 0x45, 2);
    require(decoder.state().bp(0xc0) == 0x123488 && sink.bp_addresses == std::vector<std::uint8_t>({0x45, 0x45}),
            "unobserved BP writes still apply masks and repeated observed operations retain callbacks");
    bp(decoder, 0x64, 0x1000 >> 5); bp(decoder, 0x65, 1u << 10);
    std::uint8_t palette[32];
    require(memory.reads == 1 && decoder.state().read_palette(0, palette, sizeof palette) && palette[31] == 0x5a,
            "unobserved TLUT writes still load palette memory");
}

void xf_numeric_transfers() {
    Memory memory; Sink sink; Decoder decoder(memory, sink);
    std::uint32_t random = 6789;
    for (unsigned trial = 0; trial < 500; ++trial) {
        random = random * 1664525u + 1013904223u;
        const unsigned count = 1 + random % 16;
        const unsigned address = trial % 5 == 0 ? 0xffd : (random >> 16) % (0x1000 - count);
        std::vector<std::uint8_t> bytes{0x10,0,static_cast<std::uint8_t>(count - 1),
                                       static_cast<std::uint8_t>(address >> 8),static_cast<std::uint8_t>(address)};
        std::vector<std::uint32_t> expected;
        for (unsigned i = 0; i < count; ++i) {
            random = random * 1664525u + 1013904223u; expected.push_back(random);
            bytes.insert(bytes.end(), {static_cast<std::uint8_t>(random >> 24),static_cast<std::uint8_t>(random >> 16),
                                       static_cast<std::uint8_t>(random >> 8),static_cast<std::uint8_t>(random)});
        }
        ok(decoder.feed({bytes.data(),bytes.size()}));
        for (unsigned i = 0; i < count; ++i) require(decoder.state().xf(address + i) == expected[i],
                                                   "XF endian conversion preserves every word across memory/register boundaries");
        const auto revision = decoder.state().revision();
        ok(decoder.feed({bytes.data(),bytes.size()}));
        require(decoder.state().revision() == revision, "an equal XF transfer leaves revisions unchanged");
    }
}

void indexed_list_state_footprint() {
    Memory memory; Sink sink; Decoder decoder(memory, sink);
    StarletGxStateCache cache{};
    std::uint32_t random = 98765;
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
        auto next = [&] { random = random * 1664525u + 1013904223u; return random; };
        const unsigned vat = next() & 7;
        std::uint32_t low = next() & 511, high = 0;
        low |= (2u + (next() & 1)) << 9;
        for (unsigned attribute = 1; attribute < 12; ++attribute) {
            unsigned mode = next() % 3; if (mode) ++mode;
            if (attribute < 4) low |= mode << (9 + attribute * 2);
            else high |= mode << ((attribute - 4) * 2);
        }
        auto a = 1u | (4u << 1) | (4u << 10) | (1u << 30);
        if (next() & 1) a |= 0x80000200u;
        cp(decoder, 0x50, low); cp(decoder, 0x60, high); cp(decoder, 0x70 + vat, a);
        VertexLayout layout; std::string error;
        require(decoder.state().vertex_layout(vat, layout, error), "random indexed layout is valid");
        starlet_gx_cp_note(&cache, 8, 0x50, low, 0); starlet_gx_cp_note(&cache, 8, 0x60, high, 0);
        starlet_gx_cp_note(&cache, 8, 0x70 + vat, a, 0);
        std::vector<std::uint8_t> bytes(3 + 3 * layout.stride, 0x61);
        bytes[0] = 0x90 | vat; bytes[1] = 0; bytes[2] = 3;
        const auto end = bytes.size();
        while (bytes.size() % 32) bytes.push_back(0);
        require(starlet_gx_indexed_draw_list(&cache, bytes.data(), bytes.size()),
                "indexed footprint sizes match the independent GX layout decoder");
        bytes.resize(end + 5); bytes[end] = 0x61;
        require(!starlet_gx_indexed_draw_list(&cache, bytes.data(), bytes.size()), "a trailing BP command invalidates state");
        bytes.resize(end); bytes[1] = bytes[2] = 255;
        require(!starlet_gx_indexed_draw_list(&cache, bytes.data(), bytes.size()), "a truncated primitive cannot preserve state");
        bytes[1] = 0; bytes[2] = 3;
        starlet_gx_state_forget(&cache);
        require(!starlet_gx_indexed_draw_list(&cache, bytes.data(), bytes.size()),
                "a list whose incoming CP layout is unknown cannot preserve state");
    }
}

void sdk_state_write_cache() {
    StarletGxStateCache cache{};
    for (unsigned address : {0x50u, 0x60u, 0x70u, 0x87u, 0x97u, 0xa0u, 0xbfu}) {
        require(starlet_gx_cp_keep(&cache, 8, address, 123, 0), "first CP state emits");
        require(!starlet_gx_cp_keep(&cache, 8, address, 123, 0), "equal CP state is suppressed");
        require(starlet_gx_cp_keep(&cache, 8, address, 456, 1), "recorded CP always emits without learning");
        require(!starlet_gx_cp_keep(&cache, 8, address, 123, 0), "recording preserves live CP shadow");
        starlet_gx_state_forget(&cache);
        require(starlet_gx_cp_keep(&cache, 8, address, 123, 0), "FIFO/list invalidation forgets CP state");
    }
    require(starlet_gx_cp_keep(&cache, 8, 0x30, 0, 0) && starlet_gx_cp_keep(&cache, 8, 0x30, 0, 0),
            "matrix index CP writes always restore vertex latches");
    std::uint32_t matrix[12] = {1,2,3,4,5,6,7,8,9,10,11,12};
    require(starlet_gx_matrix_keep(&cache, 0, matrix, 3, 4, 4, 0), "first position matrix emits");
    require(!starlet_gx_matrix_keep(&cache, 0, matrix, 3, 4, 4, 0), "equal position matrix is suppressed");
    require(!starlet_gx_matrix_keep(&cache, 4, matrix + 4, 2, 4, 4, 0), "overlapping equal matrix rows reuse shadow");
    require(starlet_gx_matrix_keep(&cache, 0x400, matrix, 3, 3, 4, 0), "normal matrix packs three columns per row");
    matrix[3] ^= 1;
    require(!starlet_gx_matrix_keep(&cache, 0x400, matrix, 3, 3, 4, 0), "normal shadow ignores the translation column");
    matrix[0] ^= 1;
    require(starlet_gx_matrix_keep(&cache, 0x400, matrix, 3, 3, 4, 1), "matrix recording emits without learning");
    matrix[0] ^= 1;
    require(!starlet_gx_matrix_keep(&cache, 0x400, matrix, 3, 3, 4, 0), "recording preserves the live matrix shadow");
    require(starlet_gx_matrix_keep(&cache, 1535, matrix, 3, 4, 4, 0), "out-of-range matrices stay for decoder validation");
    starlet_gx_state_forget(&cache);
    require(starlet_gx_matrix_keep(&cache, 0x400, matrix, 3, 3, 4, 0), "indexed loads and FIFO changes forget matrices");
    starlet_gx_state_forget(&cache);
    Memory a_memory, b_memory; Sink a_sink, b_sink;
    Decoder a(a_memory, a_sink), b(b_memory, b_sink);
    auto same = [&] {
        for (unsigned i = 0; i < 256; ++i) require(a.state().bp(i) == b.state().bp(i), "SDK suppression retains the complete BP image");
        for (bool konst : {false, true}) for (unsigned i = 0; i < 8; ++i)
            require(a.state().tev_register(konst, i) == b.state().tev_register(konst, i), "SDK suppression retains both TEV banks");
    };
    auto write = [&](std::uint32_t word) {
        const auto address = static_cast<std::uint8_t>(word >> 24);
        bp(a, address, word & 0xffffff);
        const bool keep = starlet_gx_bp_keep(&cache, word, 0);
        if (keep) bp(b, address, word & 0xffffff);
        same();
        return keep;
    };
    write(0xfeffffff); write(0x41000008);
    require(!write(0x41000008), "an equal full-mask draw-state write is suppressed");
    write(0xe0000123); write(0xe0800456);
    require(write(0xe0000123), "bank restoration preserves the raw BP selector used by later masked writes");
    require(!write(0xe0000123), "a repeated write to the same TEV bank is suppressed");
    write(0xfe0000ff); require(write(0xe0000155), "a masked TEV write is retained");
    require(write(0xe0000155), "a masked write invalidates the shadow value");
    for (auto address : {0x0fu, 0x45u, 0x47u, 0x48u, 0x52u, 0x64u, 0x65u, 0x66u, 0x80u, 0xbfu}) {
        const auto word = address << 24;
        require(write(word) && write(word), "sync, copies, texture and TLUT operations always emit");
    }
    write(0x41000008); starlet_gx_state_forget(&cache);
    bp(a, 0xfe, 255); bp(b, 0xfe, 255);
    require(write(0x41000008), "an unknown list's final mask cannot consume a suppressed write");
    require(write(0x41000008), "the unknown masked result is learned only after a full write");
    require(!write(0x41000008), "full-state suppression resumes after mask consumption");
    StarletGxStateCache arrays{};
    std::uint32_t cp_random = 54321;
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
        cp_random = cp_random * 1664525u + 1013904223u;
        const auto address = static_cast<std::uint8_t>(0xa0 + (cp_random & 31));
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            cp(a, address, cp_random);
            const bool keep = starlet_gx_cp_keep(&arrays, 8, address, cp_random, 0);
            if (keep) cp(b, address, cp_random);
            for (unsigned reg = 0; reg < 256; ++reg)
                require(a.state().cp(reg) == b.state().cp(reg), "array-write suppression preserves all CP registers");
            require(keep == (repeat == 0), "equal SDK array bindings and strides are suppressed");
        }
        if ((iteration & 31) == 0) starlet_gx_state_forget(&arrays);
    }
    StarletGxStateCache matrices{};
    std::uint32_t random = 0x12345678;
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
        auto next = [&] { random = random * 1664525u + 1013904223u; return random; };
        const auto address = next() % 1500;
        const unsigned rows = 2 + (next() & 1), columns = 3 + (next() & 1), stride = columns + (next() & 1);
        std::array<std::uint32_t, 16> values;
        for (auto& value : values) value = next();
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            std::vector<std::uint8_t> command{0x10};
            const auto put = [&](std::uint32_t word) { for (int shift = 24; shift >= 0; shift -= 8) command.push_back(word >> shift); };
            put(((rows * columns - 1) << 16) | address);
            for (unsigned row = 0; row < rows; ++row) for (unsigned column = 0; column < columns; ++column)
                put(values[row * stride + column]);
            ok(a.feed({command.data(), command.size()}));
            const bool keep = starlet_gx_matrix_keep(&matrices, address, values.data(), rows, columns, stride, 0);
            if (keep) ok(b.feed({command.data(), command.size()}));
            require(!std::memcmp(a.state().xf_data(), b.state().xf_data(), 1536 * 4), "random overlapping matrix suppression preserves all XF memory words");
            require(keep == (repeat == 0), "an equal multirow matrix is suppressed");
        }
        if ((iteration & 31) == 0) starlet_gx_state_forget(&matrices);
    }
    const auto generation = cache.generation;
    require(starlet_gx_bp_keep(&cache, 0x41000017, 1), "recorded display lists retain explicit state writes");
    require(cache.generation == generation && !write(0x41000008), "recording does not modify the executed-state shadow");
    require(starlet_gx_xf_keep(&cache, 0x40, 123, 0) && !starlet_gx_xf_keep(&cache, 0x40, 123, 0),
            "equal texgen writes are suppressed");
    require(starlet_gx_xf_keep(&cache, 0x18, 0, 0) && starlet_gx_xf_keep(&cache, 0x18, 0, 0),
            "matrix-index writes always restore latches");
    starlet_gx_state_forget(&cache);
    require(starlet_gx_xf_keep(&cache, 0x40, 123, 0), "FIFO and list changes invalidate texgen shadows");
    cache.generation = ~std::uint32_t{0}; starlet_gx_state_forget(&cache);
    require(write(0x41000008) && starlet_gx_xf_keep(&cache, 0x40, 123, 0), "generation wrap cannot resurrect old state");
}

int main() {
    try {
        bp_mask_and_order(); tev_register_banks(); texgen_fields(); layouts_and_streaming(); layout_revision_ignores_arrays(); indexed_xf_and_bounds(); indexed_xf_reader_hook();
        palettes_snapshot_and_reload(); lists_mutation_and_shared_state(); rejected_inputs_and_limits(); failed_memory_is_not_applied(); immutable_list_during_execution();
        callback_reentry_is_rejected(); processing_exceptions_latch(); merged_primitives(); every_split_boundary();
        host_commands_and_filtered_bp(); tev_banks_in_fed_writes(); list_fast_path_matches_general_path(); selective_bp_notifications(); sdk_state_write_cache(); indexed_list_state_footprint(); xf_numeric_transfers();
        std::cout << "GX core: 20 test groups passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "GX core failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
