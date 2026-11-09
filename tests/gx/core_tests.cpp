#include "starlet/gx/decoder.hpp"

#include <algorithm>
#include <cstdlib>
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

int main() {
    try {
        bp_mask_and_order(); tev_register_banks(); texgen_fields(); layouts_and_streaming(); layout_revision_ignores_arrays(); indexed_xf_and_bounds();
        palettes_snapshot_and_reload(); lists_mutation_and_shared_state(); rejected_inputs_and_limits(); failed_memory_is_not_applied(); immutable_list_during_execution();
        callback_reentry_is_rejected(); processing_exceptions_latch(); merged_primitives(); every_split_boundary();
        std::cout << "GX core: 15 test groups passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "GX core failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
