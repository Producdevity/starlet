#include "starlet/gx/vertex.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <type_traits>

using namespace starlet::gx;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void ok(Status status) { require(static_cast<bool>(status), status.message.c_str()); }
void ok(const VertexResult& result) { require(static_cast<bool>(result), result.message.c_str()); }
struct Memory final : MemoryReader {
    std::map<std::uint32_t, std::vector<std::uint8_t>> regions;
    std::vector<std::pair<std::uint32_t, std::size_t>> reads;
    bool throws = false;
    bool array_views = false;
    std::size_t view_calls = 0, view_limit = std::numeric_limits<std::size_t>::max();
    std::map<std::uint32_t, unsigned> native_units;
    MemoryArrayView array_view(std::uint32_t address) override {
        ++view_calls;
        if (!array_views) return {};
        auto it = regions.upper_bound(address);
        if (it == regions.begin()) return {};
        --it;
        const auto offset = std::size_t{address - it->first};
        const auto unit = native_units[it->first];
        if (offset >= it->second.size() || (unit && offset % unit)) return {};
        return {{it->second.data() + offset, std::min(view_limit, it->second.size() - offset)}, static_cast<std::uint8_t>(unit)};
    }
    bool copy(std::uint32_t address, std::uint8_t* destination, std::size_t size) override {
        if (throws) throw std::runtime_error("memory exception");
        reads.emplace_back(address, size);
        auto it = regions.upper_bound(address);
        if (it == regions.begin()) return false;
        --it;
        const auto offset = static_cast<std::size_t>(address - it->first);
        if (offset > it->second.size() || size > it->second.size() - offset) return false;
        const auto unit = native_units[it->first];
        if (!unit) std::copy_n(it->second.data() + offset, size, destination);
        else for (std::size_t i = 0; i < size; ++i) {
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
    std::vector<std::shared_ptr<const VertexSnapshot>> snapshots;
    VertexResult result;
    VertexLimits limits;
    bool reject = false;
    bool throws = false;
    bool draw(const State& state, const Draw& draw, MemoryReader& memory) override {
        if (throws) throw std::runtime_error("sink exception");
        if (reject) return false;
        result = capture_vertices(state, draw, memory, limits);
        if (result) snapshots.push_back(result.snapshot);
        return static_cast<bool>(result);
    }
    bool bp_write(const State&, std::uint8_t, std::uint32_t) override { return true; }
    bool invalidate_vertices() override { return true; }
};
struct Fixture {
    Memory memory;
    Sink sink;
    Decoder decoder{memory, sink};
    void cp(std::uint8_t address, std::uint32_t value) {
        ok(decoder.write_u8(8)); ok(decoder.write_u8(address)); ok(decoder.write_u32(value));
    }
    void xf(std::uint16_t address, std::uint32_t value) {
        ok(decoder.write_u8(0x10)); ok(decoder.write_u16(0));
        ok(decoder.write_u16(address)); ok(decoder.write_u32(value));
    }
    void configure(std::uint32_t low, std::uint32_t high, std::uint32_t a,
                   std::uint32_t b = 0, std::uint32_t c = 0) {
        cp(0x50, low); cp(0x60, high); cp(0x70, a); cp(0x80, b); cp(0x90, c);
    }
    void array(unsigned attribute, std::uint32_t base, unsigned stride, std::vector<std::uint8_t> bytes) {
        cp(static_cast<std::uint8_t>(0xa0 + attribute), base);
        cp(static_cast<std::uint8_t>(0xb0 + attribute), stride);
        memory.regions[base] = std::move(bytes);
    }
    VertexResult capture(const std::vector<std::uint8_t>& bytes, std::uint8_t primitive = 0xb8,
                         std::uint16_t count = 1, VertexLimits limits = {}, VertexCache* cache = nullptr) {
        Draw draw; draw.primitive = primitive; draw.vertex_count = count;
        std::string error;
        require(decoder.state().vertex_layout(0, draw.layout, error), error.c_str());
        draw.vertices = {bytes.data(), bytes.size()};
        return capture_vertices(decoder.state(), draw, memory, limits, cache);
    }
    void draw(std::vector<std::uint8_t> bytes, std::uint16_t count = 1, std::uint8_t primitive = 0xb8) {
        bytes.insert(bytes.begin(), {primitive, static_cast<std::uint8_t>(count >> 8), static_cast<std::uint8_t>(count)});
        ok(decoder.feed({bytes.data(), bytes.size()}));
    }
};

void numeric_formats() {
    const std::vector<std::vector<std::uint8_t>> encoded = {
        {255, 3}, {0x80, 0x7f}, {0xff, 0xff, 0, 3}, {0x80, 0, 0x7f, 0xff},
        {0xc0,0x20,0,0, 0x3f,0xc0,0,0}
    };
    const std::array<std::array<float, 2>, 5> values = {{{255,3}, {-128,127}, {65535,3}, {-32768,32767}, {-2.5f,1.5f}}};
    for (unsigned format = 0; format < 5; ++format) {
        for (unsigned mode = 1; mode <= 3; ++mode) {
            for (unsigned fractional : {0u, 5u, 31u}) {
                for (bool byte_dequant : {false, true}) {
                    Fixture f;
                    f.configure(mode << 9, mode, (format << 1) | (fractional << 4) |
                        (1u << 21) | (format << 22) | (fractional << 25) | (std::uint32_t{byte_dequant} << 30));
                    std::vector<std::uint8_t> bytes;
                    if (mode == 1) { bytes = encoded[format]; bytes.insert(bytes.end(), encoded[format].begin(), encoded[format].end()); }
                    else {
                        std::vector<std::uint8_t> array(7, 0xaa);
                        array.insert(array.end(), encoded[format].begin(), encoded[format].end());
                        f.array(0, 0x1001, 7, array); f.array(4, 0x2003, 7, array);
                        bytes = mode == 2 ? std::vector<std::uint8_t>{1,1} : std::vector<std::uint8_t>{0,1,0,1};
                    }
                    const auto result = f.capture(bytes); ok(result);
                    const auto& v = result.snapshot->vertices()[0];
                    const auto shift = format == 4 || (format < 2 && !byte_dequant) ? 0 : fractional;
                    for (unsigned component = 0; component < 2; ++component) {
                        const float expected = std::ldexp(values[format][component], -static_cast<int>(shift));
                        require(v.position[component] == expected && v.texcoords[0][component] == expected,
                                "numeric direct/indexed conversion and byte dequant");
                    }
                    require(v.position[2] == 0, "XY position fills Z with zero");
                }
            }
        }
    }
    Fixture wide; wide.configure(0x600,0,4);
    wide.array(0,0x1000,7,{});
    wide.memory.regions[0x1000+0x1234*7]={0xff,0xff,0,1};
    auto indexed16=wide.capture({0x12,0x34}); ok(indexed16);
    require(indexed16.snapshot->vertices()[0].position[0]==65535 && wide.memory.reads[0].first==0x8f6c,
            "index16 high byte is part of the array address");
    Fixture f; f.configure(0x200, 1, 9 | (4u << 22));
    const auto result = f.capture({0x80,0,0,0, 0x7f,0x80,0,0, 0x7f,0xc1,0x23,0x45, 0x3f,0,0,0}); ok(result);
    const auto& v = result.snapshot->vertices()[0];
    std::uint32_t bits = 0; std::memcpy(&bits, &v.position[2], sizeof(bits));
    require(std::signbit(v.position[0]) && std::isinf(v.position[1]) && bits == 0x7fc12345,
            "float signed zero infinity and NaN payload preserved");
    require(v.texcoords[0][0] == 0.5f && v.texcoords[0][1] == 0, "one-component texcoord");
}

void normals() {
    const std::vector<std::vector<std::uint8_t>> vector = {
        {0x80,0x40,0x7f}, {0x80,0,0x40,0,0x7f,0xff},
        {0xc0,0,0,0,0x3f,0x80,0,0,0x3f,0xc0,0,0}
    };
    const unsigned formats[] = {1,3,4};
    const std::array<std::array<float, 3>, 3> expected = {{{-2,1,127.0f/64}, {-2,1,32767.0f/16384}, {-2,1,1.5f}}};
    for (unsigned fmt = 0; fmt < 3; ++fmt) {
        for (unsigned count : {3u, 9u}) {
            for (unsigned mode = 1; mode <= 3; ++mode) {
                Fixture f; f.configure(0x200 | (mode << 11), 0, (formats[fmt] << 10) | (count == 9 ? 0x200 : 0));
                std::vector<std::uint8_t> normal;
                for (unsigned i = 0; i < count / 3; ++i) normal.insert(normal.end(), vector[fmt].begin(), vector[fmt].end());
                std::vector<std::uint8_t> bytes{1,2};
                if (mode == 1) bytes.insert(bytes.end(), normal.begin(), normal.end());
                else {
                    f.array(1, 0x3000, 0, normal);
                    if (mode == 3) bytes.push_back(0);
                    bytes.push_back(7);
                }
                auto result = f.capture(bytes); ok(result);
                for (unsigned i = 0; i < count; ++i)
                    require(result.snapshot->vertices()[0].normal[i] == expected[fmt][i % 3], "normal fixed scale and NBT tuple");
            }
        }
    }
    for (unsigned mode : {2u, 3u}) {
        Fixture f; f.configure(0x200 | (mode << 11), 0, 0x80000600);
        f.array(1, 0x4000, 12, std::vector<std::uint8_t>(48, 0x7e));
        auto& a = f.memory.regions[0x4000];
        a[24]=64; a[25]=0; a[26]=0;
        a[3]=0; a[4]=64; a[5]=0;
        a[18]=0; a[19]=0; a[20]=64;
        const auto bytes = mode == 2 ? std::vector<std::uint8_t>{1,2, 2,0,1} : std::vector<std::uint8_t>{1,2, 0,2,0,0,0,1};
        auto result = f.capture(bytes); ok(result);
        require(result.snapshot->vertices()[0].normal == std::array<float,9>{1,0,0,0,1,0,0,0,1}, "NBT3 distinct indices and N/B/T byte offsets");
        require(f.memory.reads == std::vector<std::pair<std::uint32_t,std::size_t>>{{0x4018,3},{0x4003,3},{0x4012,3}}, "NBT3 exact bounded reads");
    }
    for (unsigned fmt = 0; fmt < 3; ++fmt) {
        for (unsigned mode : {1u,2u,3u}) {
            Fixture f; f.configure(0x200 | (mode << 11),0,0x80000200 | (formats[fmt]<<10));
            std::vector<std::uint8_t> data;
            for (unsigned i=0;i<3;++i) data.insert(data.end(),vector[fmt].begin(),vector[fmt].end());
            std::vector<std::uint8_t> fifo{1,2};
            if (mode==1) fifo.insert(fifo.end(),data.begin(),data.end());
            else {
                f.array(1,0x4500,0,data);
                fifo.insert(fifo.end(),mode==2 ? 3 : 6,0);
            }
            auto result=f.capture(fifo); ok(result);
            for (unsigned i=0;i<9;++i)
                require(result.snapshot->vertices()[0].normal[i]==expected[fmt][i%3],"NBT3 flag across S8 S16 F32 and direct/indexed modes");
        }
    }
    Fixture f; f.configure(0x1200, 0, 0x400); f.array(1, 0x5000, 0, {64,0,0});
    auto result = f.capture({1,2,255}); ok(result);
    require(result.snapshot->vertices()[0].normal[0] == 1, "maximum non-position index is not a skip marker");
}

void colors() {
    const std::vector<std::vector<std::uint8_t>> bytes = {{0xa5,0xa5}, {13,79,201}, {13,79,201,7}, {0x17,0xa0}, {0x04,0x28,0x3f}, {13,79,201,23}};
    const std::array<std::array<std::uint8_t,4>,6> expected = {{{165,182,41,255}, {13,79,201,255}, {13,79,201,255},
                                                             {17,119,170,0}, {4,8,130,255}, {13,79,201,23}}};
    for (unsigned fmt = 0; fmt < 6; ++fmt) {
        for (unsigned mode = 1; mode <= 3; ++mode) {
            Fixture f; const auto components = fmt >= 3 ? 1u : 0u;
            f.configure(0x200 | (mode << 13) | (mode << 15), 0,
                        (components << 13) | (fmt << 14) | (components << 17) | (fmt << 18));
            std::vector<std::uint8_t> fifo{2,3};
            if (mode == 1) {
                fifo.insert(fifo.end(), bytes[fmt].begin(), bytes[fmt].end());
                fifo.insert(fifo.end(), bytes[fmt].begin(), bytes[fmt].end());
            } else {
                f.array(2, 0x6000, 0, bytes[fmt]); f.array(3, 0x7000, 0, bytes[fmt]);
                if (mode == 2) fifo.insert(fifo.end(), {255,3});
                else fifo.insert(fifo.end(), {255,255,0,3});
            }
            auto result = f.capture(fifo); ok(result);
            require(result.snapshot->vertices()[0].colors[0] == expected[fmt] &&
                    result.snapshot->vertices()[0].colors[1] == expected[fmt], "all packed color formats, channels, index modes");
        }
    }
}

void all_attributes_and_lifetime() {
    Fixture f;
    f.configure(0x5ff, 0xffff, 0x00200000, 0x08040201, 0x00804020);
    f.array(0, 0x8000, 2, {3,4,7,8});
    for (unsigned t = 0; t < 8; ++t) f.array(4+t, 0x9000 + t*0x100, 2, {static_cast<std::uint8_t>(t+1),static_cast<std::uint8_t>(t+11)});
    std::vector<std::uint8_t> fifo{0,3,6,9,12,15,18,21,24,1};
    for (unsigned t = 0; t < 8; ++t) fifo.insert(fifo.end(), {0,0});
    const auto original_fifo=fifo;
    auto old = f.capture(fifo); ok(old);
    auto alias = old.snapshot;
    fifo.assign(fifo.size(), 0xcc);
    for (auto& [address, data] : f.memory.regions) { (void)address; std::fill(data.begin(), data.end(), 99); }
    f.cp(0x30, 0xffffffff); f.xf(0x1018, 0x12345678);
    const auto& v = alias->vertices()[0];
    require(v.position == std::array<float,3>{7,8,0}, "owned mutable indexed position");
    require(v.matrix_indices == std::array<std::uint8_t,9>{0,3,6,9,12,15,18,21,24}, "all matrix attributes decoded in FIFO order");
    for (unsigned t = 0; t < 8; ++t)
        require(v.texcoords[t] == std::array<float,2>{float(t+1),float(t+11)}, "all eight independent texture arrays");
    auto fresh=f.capture(original_fifo); ok(fresh);
    require(fresh.snapshot->vertices()[0].position[0]==99 && alias->vertices()[0].position[0]==7,"later capture sees mutable arrays without stale caching");
    old.snapshot.reset(); require(alias->vertices()[0].position[0] == 7, "shared immutable lifetime survives producer result release");
    static_assert(std::is_same_v<decltype(alias->vertices()), const std::vector<ExpandedVertex>&>);
    Fixture direct; direct.configure(0x200,0,0);
    std::vector<std::uint8_t> source{11,12}; auto snapshot = direct.capture(source); ok(snapshot);
    source[0]=55; require(snapshot.snapshot->vertices()[0].position[0]==11, "direct FIFO bytes are owned after return");
}

void primitive_indices() {
    struct Case { std::uint8_t primitive; unsigned count; Topology topology; std::vector<std::uint16_t> expected; };
    const Case cases[] = {
        {0x80,8,Topology::triangles,{0,1,2,0,2,3,4,5,6,4,6,7}},
        {0x90,6,Topology::triangles,{0,1,2,3,4,5}},
        {0x98,6,Topology::triangles,{0,1,2,1,3,2,2,3,4,3,5,4}},
        {0xa0,5,Topology::triangles,{0,1,2,0,2,3,0,3,4}},
        {0xa8,6,Topology::lines,{0,1,2,3,4,5}},
        {0xb0,4,Topology::lines,{0,1,1,2,2,3}},
        {0xb8,3,Topology::points,{0,1,2}}
    };
    Fixture f; f.configure(0x200,0,0);
    for (const auto& c : cases) {
        auto result = f.capture(std::vector<std::uint8_t>(c.count*2,0),c.primitive,c.count); ok(result);
        require(result.snapshot->indices()==c.expected && result.snapshot->topology()==c.topology, "primitive topology and exact GX ordering");
        auto empty=f.capture({},c.primitive,0); ok(empty); require(empty.snapshot->indices().empty(),"zero-vertex draw");
    }
    auto largest=f.capture(std::vector<std::uint8_t>(65535*2,0),0x98,65535); ok(largest);
    require(largest.snapshot->indices().size()==196599 && largest.snapshot->indices().back()==65534, "maximum GX count without uint16 wrap");
    Fixture skip; skip.configure(0x400,0,0); skip.array(0,0xa000,2,{1,2,3,4,5,6});
    auto result=skip.capture({255,255,255,0,1,2},0x90,6); ok(result);
    require(result.snapshot->indices()==std::vector<std::uint16_t>{3,4,5} && skip.memory.reads.size()==3, "disabled primitive does not access indexed memory");
    auto partial=skip.capture({255,0,1},0x90,3);
    require(partial.error==VertexError::partially_disabled_primitive && !partial.snapshot,"partial disable is explicitly unsupported");
    Fixture skip16; skip16.configure(0x600,0,0);
    auto disabled=skip16.capture({255,255,255,255,255,255,255,255},0x80,4); ok(disabled);
    require(disabled.snapshot->indices().empty() && skip16.memory.reads.empty(),"16-bit skip disables whole quad without lookup");
}

void scoped_array_views() {
    Fixture f; f.configure(0x1600, 2, 0x80200600);
    std::vector<std::uint8_t> positions(64), normals(384), coordinates(64), fifo;
    for (unsigned i = 0; i < positions.size(); ++i) positions[i] = coordinates[i] = static_cast<std::uint8_t>(i);
    for (unsigned i = 0; i < normals.size(); ++i) normals[i] = static_cast<std::uint8_t>(i % 127);
    f.array(0, 0x1000, 2, positions); f.array(1, 0x2000, 12, normals); f.array(4, 0x3000, 2, coordinates);
    for (unsigned i = 0; i < 128; ++i) {
        const auto n = static_cast<std::uint8_t>(i % 32);
        fifo.insert(fifo.end(), {0, n, static_cast<std::uint8_t>((n+1)%32),
            static_cast<std::uint8_t>((n+2)%32), static_cast<std::uint8_t>((n+3)%32), n});
    }
    const auto reference = f.capture(fifo, 0xb8, 128); ok(reference);
    require(f.memory.reads.size() == 640, "copy-only control reads every indexed vector");
    const auto matches = [&](const VertexSnapshot& actual) {
        const auto& expected = *reference.snapshot;
        require(actual.indices() == expected.indices() && actual.array_bytes_read() == expected.array_bytes_read(), "view topology and logical budget unchanged");
        for (unsigned i = 0; i < expected.vertices().size(); ++i) {
            const auto& a = actual.vertices()[i]; const auto& b = expected.vertices()[i];
            require(a.position == b.position && a.normal == b.normal && a.colors == b.colors &&
                a.texcoords == b.texcoords && a.matrix_indices == b.matrix_indices && a.disabled == b.disabled,
                "array views preserve every attribute including NBT3 offsets");
        }
    };
    f.memory.reads.clear(); f.memory.view_calls = 0; f.memory.array_views = true;
    const auto viewed = f.capture(fifo, 0xb8, 128); ok(viewed); matches(*viewed.snapshot);
    require(f.memory.reads.empty() && f.memory.view_calls == 3, "one scoped view per consumed array replaces 640 copies");
    f.memory.view_limit = 2;
    const auto partial = f.capture(fifo, 0xb8, 128); ok(partial); matches(*partial.snapshot);
    require(!f.memory.reads.empty(), "requests outside a partial view fall back to bounded copies");
    f.memory.view_limit = std::numeric_limits<std::size_t>::max();
    f.memory.regions[0x1000][0] = 201;
    const auto changed = f.capture(fifo, 0xb8, 128); ok(changed);
    require(changed.snapshot->vertices()[0].position[0] == 201 && viewed.snapshot->vertices()[0].position[0] == 0,
            "each draw observes mutations and retained snapshots own their values");
    VertexLimits limited; limited.max_array_bytes = 128 * 13 - 1;
    require(f.capture(fifo, 0xb8, 128, limited).error == VertexError::read_limit, "views cannot bypass logical limits");
    Fixture skipped; skipped.configure(0x400,0,0); skipped.memory.array_views = true;
    ok(skipped.capture(std::vector<std::uint8_t>(128,255),0xb8,128));
    require(skipped.memory.view_calls == 0 && skipped.memory.reads.empty(), "disabled vertices borrow no arrays");

    for (unsigned format : {2u,3u,4u}) {
        Fixture native; native.configure(0x600,0,1|(format<<1)|(7u<<4));
        const auto unit = format == 4 ? 4u : 2u;
        std::vector<std::uint8_t> data(4*3*unit), indices;
        for (unsigned i=0;i<12;++i) {
            if (unit==4) { const float value=(float(i)-6)*.25f; std::memcpy(data.data()+i*unit,&value,unit); }
            else { const std::uint16_t value=i%2 ? std::uint16_t(32768+i) : std::uint16_t(i); std::memcpy(data.data()+i*unit,&value,unit); }
        }
        native.array(0,0x1000,3*unit,data); native.memory.native_units[0x1000]=unit;
        for(unsigned i=0;i<64;++i)indices.insert(indices.end(),{0,static_cast<std::uint8_t>(i%4)});
        const auto copied=native.capture(indices,0xb8,64);ok(copied);
        native.memory.reads.clear();native.memory.array_views=true;
        const auto direct=native.capture(indices,0xb8,64);ok(direct);
        require(native.memory.reads.empty(),"matching native numeric views avoid conversion copies");
        for(unsigned i=0;i<64;++i)require(direct.snapshot->vertices()[i].position==copied.snapshot->vertices()[i].position,"native U16/S16/F32 values match canonical decoding");
    }
    Fixture bits;bits.configure(0x400,0,1|(4u<<1));
    const std::array<std::uint32_t,3> payload{{0x80000000,0x7f800000,0x7fc00051}};
    std::vector<std::uint8_t> bytes(sizeof(payload));std::memcpy(bytes.data(),payload.data(),bytes.size());
    bits.array(0,0x5000,12,bytes);bits.memory.native_units[0x5000]=4;bits.memory.array_views=true;
    const auto special=bits.capture({0});ok(special);
    require(!std::memcmp(special.snapshot->vertices()[0].position.data(),payload.data(),sizeof(payload)),"native float view preserves signed zero infinity and NaN payload bits");
}

void verified_geometry_cache() {
    Fixture f;f.configure(0x400,0,0);f.array(0,0x1000,2,{1,2,3,4});f.memory.array_views=true;
    VertexCache cache;
    const auto capture=[&](std::vector<std::uint8_t> fifo,VertexLimits limits=VertexLimits{}) {
        return f.capture(fifo,0xb8,static_cast<std::uint16_t>(fifo.size()),limits,&cache);
    };
    const auto first=capture({0,1,0});ok(first);
    const auto again=capture({0,1,0});ok(again);
    require(&first.snapshot->vertices()==&again.snapshot->vertices() && first.snapshot!=again.snapshot &&
            cache.stats().hits==1 && cache.stats().entries==1,"verified hit shares immutable geometry through a fresh snapshot");
    f.xf(0x1009,1);
    const auto revised=capture({0,1,0});ok(revised);
    require(revised.snapshot->state_revision()==f.decoder.state().revision() &&
            revised.snapshot->state_revision()!=first.snapshot->state_revision() &&
            &revised.snapshot->vertices()==&first.snapshot->vertices(),"unrelated state revisions reuse geometry with current revision");
    VertexLimits limit;limit.max_array_bytes=5;
    require(capture({0,1,0},limit).error==VertexError::read_limit,"hit preserves repeated logical read budget");
    limit={};limit.max_vertices=2;require(capture({0,1,0},limit).error==VertexError::storage_limit,"hit enforces vertex limit");
    limit={};limit.max_indices=2;require(capture({0,1,0},limit).error==VertexError::storage_limit,"hit enforces index limit");
    limit={};limit.max_storage_bytes=first.snapshot->storage_bytes()-1;
    require(capture({0,1,0},limit).error==VertexError::storage_limit,"hit enforces storage limit");
    f.memory.regions[0x1000][0]=9;
    const auto mutated=capture({0,1,0});ok(mutated);
    require(mutated.snapshot->vertices()[0].position[0]==9 && first.snapshot->vertices()[0].position[0]==1 &&
            &mutated.snapshot->vertices()!=&first.snapshot->vertices() && cache.stats().entries==1 && cache.stats().invalidations==1,"same-address changes replace stale content without altering retained snapshots");
    const auto fifo=capture({1,0,1});ok(fifo);require(fifo.snapshot->vertices()[0].position[0]==3,"FIFO changes invalidate geometry");
    f.xf(0x1018,6);const auto matrix=capture({0,1,0});ok(matrix);
    require(matrix.snapshot->vertices()[0].matrix_indices[0]==6,"implicit matrix latches participate in the key");
    f.cp(0xb0,0);const auto stride=capture({0,1,0});ok(stride);
    require(stride.snapshot->vertices()[1].position[0]==9,"array stride changes participate in the key");
    f.memory.regions.clear();require(capture({0,1,0}).error==VertexError::memory_read,"retired arrays cannot hit retained geometry");
    f.array(0,0x1000,2,{7,8,3,4});const auto rebound=capture({0,1,0});ok(rebound);
    require(rebound.snapshot->vertices()[0].position[0]==7,"reused source address observes its new bytes");
    f.memory.array_views=false;VertexCache fallback;
    ok(f.capture({0},0xb8,1,{},&fallback));ok(f.capture({0},0xb8,1,{},&fallback));
    require(fallback.stats().entries==0 && fallback.stats().hits==0,"fallback copies do not create unverifiable cache entries");
    f.memory.array_views=true;f.memory.view_limit=1;VertexCache partial;
    ok(f.capture({0},0xb8,1,{},&partial));require(partial.stats().entries==0,"partial views cannot hide fallback dependencies");
    std::shared_ptr<const VertexSnapshot> retained;
    { VertexCache tiny({4096,1});f.memory.view_limit=100;
      const auto a=f.capture({0},0xb8,1,{},&tiny);ok(a);retained=a.snapshot;
      const auto b=f.capture({1},0xb8,1,{},&tiny);ok(b);
      require(tiny.stats().entries==1 && tiny.stats().bytes<=4096 && tiny.stats().evictions==1,"LRU entry and byte budgets remain bounded"); }
    require(retained->vertices()[0].position[0]==7,"cache eviction and destruction preserve caller-owned geometry");
    for(VertexCacheLimits bounds: {VertexCacheLimits{0,0},VertexCacheLimits{1,1}}) {
        VertexCache disabled(bounds);ok(f.capture({0},0xb8,1,{},&disabled));
        require(disabled.stats().entries==0 && disabled.stats().bytes==0,"undersized or disabled caches retain no payload");
    }
    Fixture native;native.configure(0x400,0,1|(4u<<1));native.memory.array_views=true;
    const std::array<float,3> values{{1,2,3}};std::vector<std::uint8_t> data(sizeof(values));std::memcpy(data.data(),values.data(),data.size());
    native.array(0,0x2000,12,data);native.memory.native_units[0x2000]=4;VertexCache native_cache;
    const auto n=native.capture({0},0xb8,1,{},&native_cache);ok(n);
    const auto h=native.capture({0},0xb8,1,{},&native_cache);ok(h);
    require(&n.snapshot->vertices()==&h.snapshot->vertices(),"native F32 sources can hit without canonical conversion");
    native.memory.native_units[0x2000]=0;const auto encoding=native.capture({0},0xb8,1,{},&native_cache);ok(encoding);
    require(&encoding.snapshot->vertices()!=&n.snapshot->vertices() && encoding.snapshot->vertices()[0].position[0]!=1,"source encoding changes invalidate equal raw bytes");
    for(unsigned mode:{2u,3u}) {
        Fixture nbt;nbt.configure(0x200|(mode<<11),0,0x80000600);nbt.memory.array_views=true;
        nbt.array(1,0x4000,12,std::vector<std::uint8_t>(48,0x7e));
        auto& a=nbt.memory.regions[0x4000];a[24]=64;a[25]=0;a[26]=0;a[3]=0;a[4]=64;a[5]=0;a[18]=0;a[19]=0;a[20]=64;
        const auto fifo=mode==2?std::vector<std::uint8_t>{1,2,2,0,1}:std::vector<std::uint8_t>{1,2,0,2,0,0,0,1};
        VertexCache cache;const auto first=nbt.capture(fifo,0xb8,1,{},&cache);ok(first);
        const auto hit=nbt.capture(fifo,0xb8,1,{},&cache);ok(hit);
        require(&first.snapshot->vertices()==&hit.snapshot->vertices() && hit.snapshot->vertices()[0].normal==std::array<float,9>{1,0,0,0,1,0,0,0,1},"NBT3 cache preserves separate indices and vector offsets");
        a[19]=32;const auto changed=nbt.capture(fifo,0xb8,1,{},&cache);ok(changed);
        require(changed.snapshot->vertices()[0].normal[7]==.5f && first.snapshot->vertices()[0].normal[7]==0,"NBT3 dependencies include the third indexed vector");
    }
    Fixture direct;direct.configure(0x200,0,0);VertexCache direct_cache;
    const auto d=direct.capture({1,2,3,4},0xa8,2,{},&direct_cache);ok(d);
    const auto e=direct.capture({1,2,3,4},0xa8,2,{},&direct_cache);ok(e);
    require(direct_cache.stats().hits==1 && &d.snapshot->indices()==&e.snapshot->indices(),"direct geometry needs no array source");
    const auto points=direct.capture({1,2,3,4},0xb8,2,{},&direct_cache);ok(points);
    require(points.snapshot->topology()==Topology::points && d.snapshot->topology()==Topology::lines,"primitive topology participates in identity");
}

void limits_and_failures() {
    Fixture f; f.configure(0x400,0,0); f.array(0,0xfffffff0,255,{1,2});
    auto overflow=f.capture({1}); require(overflow.error==VertexError::memory_read && f.memory.reads.empty(),"address overflow before read");
    f.array(0,0xffffffff,0,{1});
    require(f.capture({0}).error==VertexError::memory_read && f.memory.reads.empty(),"range end overflow before read");
    f.array(0,0xfffffffE,0,{1,2}); ok(f.capture({0}));
    f.array(0,0x1000,1,{1});
    require(f.capture({0}).error==VertexError::memory_read,"undersized array");
    f.array(0,0x1000,0,{1,2});
    VertexLimits limit; limit.max_array_bytes=1;
    require(f.capture({0},0xb8,1,limit).error==VertexError::read_limit,"array byte budget");
    const auto reads=f.memory.reads.size();
    limit={}; limit.max_vertices=0; require(f.capture({0},0xb8,1,limit).error==VertexError::storage_limit,"vertex budget");
    limit={}; limit.max_indices=0; require(f.capture({0},0xb8,1,limit).error==VertexError::storage_limit,"index budget");
    limit={}; limit.max_storage_bytes=0; require(f.capture({0},0xb8,1,limit).error==VertexError::storage_limit,"storage budget");
    require(f.memory.reads.size()==reads,"storage checks precede memory reads");
    require(f.capture({}).error==VertexError::invalid_input && f.capture({0,0}).error==VertexError::invalid_input,"exact FIFO byte count");
    require(f.capture({0},0x88).error==VertexError::invalid_primitive,"reserved primitive rejected");
    for (auto opcode : {0x80,0x90,0x98,0xa0,0xa8,0xb0})
        require(f.capture({0},static_cast<std::uint8_t>(opcode)).error==VertexError::partial_primitive,"incomplete primitive count");
    Draw draw; std::string error; require(f.decoder.state().vertex_layout(0,draw.layout,error),"valid layout");
    draw.primitive=0xb8; draw.vertex_count=1; draw.vertices={nullptr,1};
    require(capture_vertices(f.decoder.state(),draw,f.memory).error==VertexError::invalid_input,"null FIFO bytes");
    const std::uint8_t data=0; draw.vertices={&data,1}; draw.layout.position.fifo_bytes=999;
    require(capture_vertices(f.decoder.state(),draw,f.memory).error==VertexError::invalid_layout,"forged layout rejected");
    f.memory.throws=true; bool threw=false;
    try { f.capture({0}); } catch (const std::runtime_error&) { threw=true; }
    require(threw,"memory exceptions propagate without a published snapshot");
    Fixture mismatch; mismatch.configure(0x2200,0,5u<<14);
    require(mismatch.capture({0,0,1,2,3,4}).error==VertexError::unsupported_layout,"inconsistent color count explicit rejection");
}

void matrix_latches() {
    Fixture f; f.configure(0x203,0,0);
    f.cp(0x30,3|(6<<6)); f.xf(0x1018,3|(6<<6)); f.xf(0x1019,21);
    f.draw({9,12,1,2,15,18,3,4},2);
    require(f.sink.snapshots[0]->vertices()[0].matrix_indices[0]==9 &&
            f.sink.snapshots[0]->vertices()[1].matrix_indices[0]==15,"per-vertex matrix selection");
    require(f.decoder.state().matrix_index(0)==15 && f.decoder.state().matrix_index(1)==18,"draw commits final effective indices");
    require(f.decoder.state().cp(0x30)==std::uint32_t(3|(6<<6)) && f.decoder.state().xf(0x1018)==std::uint32_t(3|(6<<6)),"draw does not forge raw CP/XF register writes");
    f.cp(0x50,0x200); f.draw({1,2});
    require(f.sink.snapshots[0]->vertices()[0].matrix_indices[5]==21,"XF matrix index bank B is captured");
    require(f.sink.snapshots.back()->vertices()[0].matrix_indices[0]==15 &&
            f.sink.snapshots.back()->vertices()[0].matrix_indices[1]==18,"absent indices use current canonical latches");
    f.cp(0x30,27); f.draw({1,2});
    require(f.sink.snapshots.back()->vertices()[0].matrix_indices[0]==15,"CP register image is separate from effective XF latches");
    f.xf(0x1018,27); f.draw({1,2});
    require(f.sink.snapshots.back()->vertices()[0].matrix_indices[0]==27,"paired SDK current-matrix write replaces latch");
    f.cp(0x50,0x201); f.draw({42,1,2});
    require(f.decoder.state().matrix_index(0)==42,"indexed draw latches its last matrix index");
    const auto before=f.decoder.state().revision();
    f.xf(0x1018,27);
    require(f.decoder.state().matrix_index(0)==27 && f.decoder.state().revision()!=before,
            "rewriting the unchanged current-matrix register still replaces the latch");
    f.cp(0x50,0x200); f.draw({1,2});
    require(f.sink.snapshots.back()->vertices()[0].matrix_indices[0]==27,"later non-indexed draw uses the restored index");
    f.cp(0x50,0x201); f.draw({},0);
    require(f.decoder.state().matrix_index(0)==27,"zero-vertex draw leaves latch");
    f.cp(0x50,0x401); f.draw({33,255});
    require(f.decoder.state().matrix_index(0)==27,"disabled vertex does not reach XF");
    f.cp(0x50,0x201);
    f.memory.regions[0xb000]={0xb8,0,1,42,1,2};
    ok(f.decoder.write_u8(0x40)); ok(f.decoder.write_u32(0xb000)); ok(f.decoder.write_u32(6));
    f.cp(0x50,0x200); f.draw({1,2});
    require(f.sink.snapshots.back()->vertices()[0].matrix_indices[0]==42,"display list and direct commands share latch");
    Fixture rejected; rejected.configure(0x201,0,0); rejected.xf(0x1018,6); rejected.sink.reject=true;
    const std::uint8_t command[]={0xb8,0,1,9,1,2};
    require(rejected.decoder.feed({command,sizeof(command)}).error==Error::sink_rejected &&
            rejected.decoder.state().matrix_index(0)==6,"rejected draw does not commit latch");
    Fixture exception; exception.configure(0x201,0,0); exception.xf(0x1018,6); exception.sink.throws=true;
    bool threw=false; try { exception.decoder.feed({command,sizeof(command)}); } catch (const std::runtime_error&) { threw=true; }
    require(threw && exception.decoder.status().error==Error::processing_exception && exception.decoder.state().matrix_index(0)==6,"throwing draw does not commit latch");
}
}
int main() {
    try {
        numeric_formats(); normals(); colors(); all_attributes_and_lifetime(); primitive_indices(); scoped_array_views(); verified_geometry_cache(); limits_and_failures(); matrix_latches();
        std::cout << "GX vertex expansion and immutable geometry snapshots passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
