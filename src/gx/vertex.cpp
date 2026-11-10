#include "starlet/gx/vertex.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <list>
#include <map>

namespace starlet::gx {
namespace {
constexpr std::size_t numeric_size[] = {1, 1, 2, 2, 4};
constexpr std::size_t color_size[] = {2, 3, 4, 2, 3, 4};

std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((std::uint16_t{p[0]} << 8) | p[1]);
}
std::uint32_t be24(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
}
std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
           (std::uint32_t{p[2]} << 8) | p[3];
}
bool indexed(const Attribute& a) {
    return a.mode == AttributeMode::index8 || a.mode == AttributeMode::index16;
}
std::uint16_t index(const Attribute& a, const std::uint8_t* p) {
    return a.mode == AttributeMode::index8 ? p[0] : be16(p);
}
bool equal(const Attribute& a, const Attribute& b) {
    return a.mode == b.mode && a.components == b.components && a.format == b.format &&
           a.fraction_bits == b.fraction_bits && a.indices == b.indices && a.fifo_bytes == b.fifo_bytes;
}
bool equal(const VertexLayout& a, const VertexLayout& b) {
    if (a.stride != b.stride || a.byte_dequant != b.byte_dequant || a.matrix_indices != b.matrix_indices ||
        !equal(a.position, b.position) || !equal(a.normal, b.normal)) {
        return false;
    }
    for (unsigned i = 0; i < a.colors.size(); ++i) {
        if (!equal(a.colors[i], b.colors[i])) return false;
    }
    for (unsigned i = 0; i < a.texcoords.size(); ++i) {
        if (!equal(a.texcoords[i], b.texcoords[i])) return false;
    }
    return true;
}
VertexResult failure(VertexError error, const char* message) { return {error, message, {}}; }

void numeric(const std::uint8_t* p, unsigned format, unsigned count, unsigned fraction, float* out,
             bool native_endian = false) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    if (format == 4) {
        if (native_endian) std::memcpy(out, p, count * sizeof(float));
        else for (unsigned i = 0; i < count; ++i) {
            const auto word = be32(p + i * 4);
            std::memcpy(out + i, &word, sizeof(word));
        }
        return;
    }
    const float scale = std::ldexp(1.0f, -static_cast<int>(fraction));
    for (unsigned i = 0; i < count; ++i, p += numeric_size[format]) {
        switch (format) {
        case 0: out[i] = p[0] * scale; break;
        case 1: out[i] = (p[0] < 128 ? int{p[0]} : int{p[0]} - 256) * scale; break;
        case 2: case 3: {
            std::uint16_t word;
            if (native_endian) std::memcpy(&word, p, sizeof(word));
            else word = be16(p);
            const int value = format == 3 && word >= 32768 ? int{word} - 65536 : int{word};
            out[i] = value * scale;
            break;
        }
        }
    }
}
std::uint8_t expand(unsigned value, unsigned width) {
    return static_cast<std::uint8_t>((value << (8 - width)) | (value >> (2 * width - 8)));
}
void color(const std::uint8_t* p, unsigned format, std::array<std::uint8_t, 4>& out) {
    switch (format) {
    case 0: {
        const auto word = be16(p);
        out = {expand(word >> 11, 5), expand((word >> 5) & 63, 6), expand(word & 31, 5), 255};
        break;
    }
    case 1: case 2: out = {p[0], p[1], p[2], 255}; break;
    case 3: {
        const auto word = be16(p);
        out = {expand(word >> 12, 4), expand((word >> 8) & 15, 4),
               expand((word >> 4) & 15, 4), expand(word & 15, 4)};
        break;
    }
    case 4: {
        const auto word = be24(p);
        out = {expand(word >> 18, 6), expand((word >> 12) & 63, 6),
               expand((word >> 6) & 63, 6), expand(word & 63, 6)};
        break;
    }
    case 5: out = {p[0], p[1], p[2], p[3]}; break;
    }
}

struct ArrayReader {
    const State& state;
    MemoryReader& memory;
    std::size_t remaining;
    VertexError error = VertexError::none;
    std::array<std::uint8_t, 36> buffer{};
    struct Array {
        MemoryArrayView view;
        bool initialized = false;
        std::size_t first = std::numeric_limits<std::size_t>::max(), end = 0;
    };
    bool cacheable = true;
    std::array<Array, 12> arrays{};
    struct Data { const std::uint8_t* bytes = nullptr; bool native_endian = false; };

    Data read(unsigned array, std::uint16_t element, std::size_t offset, std::size_t size,
              unsigned native_unit = 0) {
        if (size > remaining) {
            error = VertexError::read_limit;
            return {};
        }
        const auto base = state.cp(static_cast<std::uint8_t>(0xa0 + array));
        const auto relative = std::uint64_t{element} * (state.cp(static_cast<std::uint8_t>(0xb0 + array)) & 255) + offset;
        const std::uint64_t address = std::uint64_t{base} + relative;
        if (address + size > (std::uint64_t{1} << 32)) {
            error = VertexError::memory_read;
            return {};
        }
        auto& cached = arrays[array];
        if (!cached.initialized) { cached.view = memory.array_view(base); cached.initialized = true; }
        const auto& view = cached.view;
        const unsigned view_unit = view.host_order ? native_unit : view.native_unit;
        if (view.bytes.data && relative <= view.bytes.size && size <= view.bytes.size - relative &&
            (!view_unit || (view_unit == native_unit && relative % native_unit == 0 && size % native_unit == 0))) {
            cached.first = std::min(cached.first, static_cast<std::size_t>(relative));
            cached.end = std::max(cached.end, static_cast<std::size_t>(relative + size));
            remaining -= size;
            return {view.bytes.data + relative, view_unit != 0};
        }
        cacheable = false;
        if (!memory.copy(static_cast<std::uint32_t>(address), buffer.data(), size)) {
            error = VertexError::memory_read;
            return {};
        }
        remaining -= size;
        return {buffer.data(), false};
    }
};

bool numbers(ArrayReader& arrays, const Attribute& a, unsigned array, const std::uint8_t*& cursor,
             bool byte_dequant, bool normal, float* out) {
    if (a.mode == AttributeMode::none) return true;
    const unsigned fraction = !normal && a.format < 2 && !byte_dequant ? 0 : a.fraction_bits;
    if (indexed(a)) {
        const unsigned vectors = a.indices;
        const unsigned components = a.components / vectors;
        const auto bytes = components * numeric_size[a.format];
        for (unsigned v = 0; v < vectors; ++v) {
            // NBT3 indices address an interleaved N/B/T record, not three identical vector arrays.
            const auto unit = a.format == 4 ? 4u : a.format == 2 || a.format == 3 ? 2u : 0u;
            const auto data = arrays.read(array, index(a, cursor), v * bytes, bytes, unit);
            cursor += a.mode == AttributeMode::index8 ? 1 : 2;
            if (!data.bytes) return false;
            numeric(data.bytes, a.format, components, fraction, out + v * components, data.native_endian);
        }
    } else {
        numeric(cursor, a.format, a.components, fraction, out);
        cursor += a.fifo_bytes;
    }
    return true;
}

bool primitive_size(std::uint8_t primitive, std::size_t n, Topology& topology, std::size_t& count) {
    topology = Topology::triangles;
    switch (primitive) {
    case 0x80: count = (n / 4) * 6; return n % 4 == 0;
    case 0x90: count = n; return n % 3 == 0;
    case 0x98: case 0xa0: count = n >= 3 ? (n - 2) * 3 : 0; return n == 0 || n >= 3;
    case 0xa8: topology = Topology::lines; count = n; return n % 2 == 0;
    case 0xb0: topology = Topology::lines; count = n >= 2 ? (n - 1) * 2 : 0; return n == 0 || n >= 2;
    case 0xb8: topology = Topology::points; count = n; return true;
    default: return false;
    }
}
}

struct VertexCache::Impl {
    using Key = std::array<std::uint32_t,39>;
    struct Span {
        std::uint32_t base = 0;
        std::size_t offset = 0;
        std::uint8_t native_unit = 0;
        bool host_order = false;
        std::vector<std::uint8_t> bytes;
    };
    struct Entry {
        Key key;
        std::uint64_t hash;
        std::vector<std::uint8_t> fifo;
        std::array<Span,12> arrays;
        std::shared_ptr<const VertexSnapshot> snapshot;
        std::size_t bytes = 0;
    };
    using Entries = std::list<Entry>;
    VertexCacheLimits limits;
    VertexCacheStats stats;
    Entries entries;
    std::multimap<std::uint64_t, Entries::iterator> index;
    explicit Impl(VertexCacheLimits value):limits(value) {}
    static Key key(const State& state,const Draw& draw) {
        Key result{};
        unsigned n=0;
        result[n++]=draw.primitive | (std::uint32_t{draw.vat}<<8) | (std::uint32_t{draw.vertex_count}<<16);
        for(unsigned address:{0x50u,0x60u,0x70u+draw.vat,0x80u+draw.vat,0x90u+draw.vat})
            result[n++]=state.cp(static_cast<std::uint8_t>(address));
        for(unsigned i=0;i<9;++i)result[n++]=draw.layout.matrix_indices[i]?0:state.matrix_index(static_cast<std::uint8_t>(i));
        const auto& l=draw.layout;
        const std::array<const Attribute*,12> attributes{{&l.position,&l.normal,&l.colors[0],&l.colors[1],
            &l.texcoords[0],&l.texcoords[1],&l.texcoords[2],&l.texcoords[3],&l.texcoords[4],&l.texcoords[5],&l.texcoords[6],&l.texcoords[7]}};
        for(unsigned i=0;i<12;++i) {
            result[n++]=indexed(*attributes[i])?state.cp(static_cast<std::uint8_t>(0xa0+i)):0;
            result[n++]=indexed(*attributes[i])?state.cp(static_cast<std::uint8_t>(0xb0+i))&255:0;
        }
        return result;
    }
    static std::uint64_t hash(const Key& key, ByteView fifo) {
        std::uint64_t result=14695981039346656037ull;
        const auto add=[&](const std::uint8_t* p,std::size_t bytes){
            for(std::size_t i=0;i<bytes;++i){result^=p[i];result*=1099511628211ull;}
        };
        add(reinterpret_cast<const std::uint8_t*>(key.data()),sizeof(key));add(fifo.data,fifo.size);
        return result;
    }
    void erase(Entries::iterator entry) {
        const auto range=index.equal_range(entry->hash);
        for(auto it=range.first;it!=range.second;++it)if(it->second==entry){index.erase(it);break;}
        stats.bytes-=entry->bytes;entries.erase(entry);stats.entries=entries.size();
    }
    std::shared_ptr<const VertexSnapshot> find(const Key& key,std::uint64_t hash,ByteView fifo,MemoryReader& memory) {
        const auto range=index.equal_range(hash);
        for(auto it=range.first;it!=range.second;++it) {
            const auto entry=it->second;
            if(entry->key!=key || entry->fifo.size()!=fifo.size ||
               (fifo.size && std::memcmp(entry->fifo.data(),fifo.data,fifo.size)))continue;
            bool matches=true;
            for(const auto& span:entry->arrays)if(!span.bytes.empty()) {
                const auto view=memory.array_view(span.base);
                if(!view.bytes.data || view.native_unit!=span.native_unit || view.host_order!=span.host_order ||
                   span.offset>view.bytes.size ||
                   span.bytes.size()>view.bytes.size-span.offset ||
                   std::memcmp(span.bytes.data(),view.bytes.data+span.offset,span.bytes.size())){matches=false;break;}
            }
            if(matches){entries.splice(entries.begin(),entries,entry);++stats.hits;return entry->snapshot;}
            ++stats.invalidations;erase(entry);break;
        }
        ++stats.misses;return {};
    }
    void insert(const Key& key,std::uint64_t hash,const Draw& draw,const ArrayReader& arrays,
                std::shared_ptr<const VertexSnapshot> snapshot) {
        if(!arrays.cacheable || !limits.entries)return;
        const auto fixed=sizeof(Entry)+sizeof(decltype(index)::value_type)+snapshot->storage_bytes();
        if(fixed>limits.bytes || draw.vertices.size>limits.bytes-fixed)return;
        std::size_t bytes=fixed+draw.vertices.size;
        for(const auto& array:arrays.arrays)if(array.end) {
            const auto size=array.end-array.first;
            if(size>limits.bytes-bytes)return;
            bytes+=size;
        }
        Entry entry;entry.key=key;entry.hash=hash;entry.snapshot=std::move(snapshot);
        if(draw.vertices.size)entry.fifo.assign(draw.vertices.data,draw.vertices.data+draw.vertices.size);
        for(unsigned i=0;i<12;++i)if(arrays.arrays[i].end) {
            const auto& source=arrays.arrays[i];auto& span=entry.arrays[i];
            span.base=arrays.state.cp(static_cast<std::uint8_t>(0xa0+i));span.offset=source.first;span.native_unit=source.view.native_unit;
            span.host_order=source.view.host_order;
            span.bytes.assign(source.view.bytes.data+source.first,source.view.bytes.data+source.end);
        }
        entry.bytes=fixed+entry.fifo.capacity();
        for(const auto& span:entry.arrays)entry.bytes+=span.bytes.capacity();
        if(entry.bytes>limits.bytes)return;
        while(!entries.empty() && (entries.size()>=limits.entries || stats.bytes>limits.bytes-entry.bytes)){++stats.evictions;erase(std::prev(entries.end()));}
        entries.push_front(std::move(entry));
        try{index.emplace(hash,entries.begin());}catch(...){entries.pop_front();throw;}
        stats.bytes+=entries.front().bytes;stats.entries=entries.size();
    }
};
VertexCache::VertexCache(VertexCacheLimits limits):impl_(std::make_unique<Impl>(limits)) {}
VertexCache::~VertexCache() = default;
VertexCacheStats VertexCache::stats() const { return impl_->stats; }

unsigned latest_normal(const State& state, const Draw& draw, MemoryReader& memory, std::array<float, 9>& normal) {
    const auto& layout = draw.layout;
    if (layout.normal.mode == AttributeMode::none || !layout.stride || !draw.vertices.data) return 0;
    ArrayReader arrays{state, memory, 64};
    for (std::size_t i = draw.vertices.size / layout.stride; i-- > 0;) {
        const auto* cursor = draw.vertices.data + i * layout.stride;
        for (bool present : layout.matrix_indices) cursor += present ? 1 : 0;
        if (indexed(layout.position) &&
            index(layout.position, cursor) == (layout.position.mode == AttributeMode::index8 ? 255 : 65535)) continue;
        cursor += layout.position.fifo_bytes;
        std::array<float, 9> read{};
        if (!numbers(arrays, layout.normal, 1, cursor, layout.byte_dequant, true, read.data())) return 0;
        const unsigned count = layout.normal.components == 9 ? 9 : 3;
        std::copy_n(read.begin(), count, normal.begin());
        return count;
    }
    return 0;
}

VertexResult capture_vertices(const State& state, const Draw& draw, MemoryReader& memory, VertexLimits limits) {
    return capture_vertices(state,draw,memory,limits,nullptr);
}

VertexResult capture_vertices(const State& state, const Draw& draw, MemoryReader& memory, VertexLimits limits, VertexCache* cache) {
    VertexLayout layout;
    std::string error;
    if (!state.vertex_layout(draw.vat, layout, error)) {
        return {VertexError::invalid_layout, std::move(error), {}};
    }
    if (!equal(layout, draw.layout)) {
        return failure(VertexError::invalid_layout, "GX draw layout does not match its state and VAT");
    }
    if (draw.merged()) {
        return failure(VertexError::invalid_input, "GX merged draws are read by the stream renderer only");
    }
    if ((draw.vertices.size && !draw.vertices.data) || draw.vertices.size != layout.stride * draw.vertex_count) {
        return failure(VertexError::invalid_input, "GX draw byte count does not match its vertex layout");
    }
    for (const auto& a : layout.colors) {
        if (a.mode != AttributeMode::none && ((a.format < 3) != (a.components == 3))) {
            return failure(VertexError::unsupported_layout, "GX color format and component count disagree");
        }
    }
    Topology topology;
    std::size_t index_count = 0;
    if (draw.primitive != 0x80 && draw.primitive != 0x90 && draw.primitive != 0x98 &&
        draw.primitive != 0xa0 && draw.primitive != 0xa8 && draw.primitive != 0xb0 && draw.primitive != 0xb8) {
        return failure(VertexError::invalid_primitive, "unsupported GX primitive opcode");
    }
    if (!primitive_size(draw.primitive, draw.vertex_count, topology, index_count)) {
        return failure(VertexError::partial_primitive, "GX draw has an incomplete primitive");
    }
    const auto storage = sizeof(VertexSnapshot) + sizeof(VertexSnapshot::Geometry) + std::size_t{draw.vertex_count} * sizeof(ExpandedVertex) +
                         index_count * sizeof(std::uint16_t);
    if (draw.vertex_count > limits.max_vertices || index_count > limits.max_indices || storage > limits.max_storage_bytes) {
        return failure(VertexError::storage_limit, "GX vertex snapshot exceeds storage limit");
    }
    VertexCache::Impl::Key key{};
    std::uint64_t hash=0;
    if(cache) {
        key=VertexCache::Impl::key(state,draw);hash=VertexCache::Impl::hash(key,draw.vertices);
        if(auto found=cache->impl_->find(key,hash,draw.vertices,memory)) {
            if(found->storage_bytes()>limits.max_storage_bytes)
                return failure(VertexError::storage_limit,"GX vertex snapshot exceeds storage limit");
            if(found->array_bytes_read()>limits.max_array_bytes)
                return failure(VertexError::read_limit,"GX indexed vertex data unavailable or read limit exceeded");
            auto result=std::shared_ptr<VertexSnapshot>(new VertexSnapshot(*found));
            result->state_revision_=state.revision();
            return {VertexError::none,{},std::move(result)};
        }
    }
    auto geometry=std::make_shared<VertexSnapshot::Geometry>();
    auto snapshot = std::unique_ptr<VertexSnapshot>(new VertexSnapshot);
    snapshot->geometry_=geometry;
    snapshot->layout_ = layout;
    snapshot->primitive_ = draw.primitive;
    snapshot->vat_ = draw.vat;
    snapshot->topology_ = topology;
    snapshot->state_revision_ = state.revision();
    geometry->vertices.resize(draw.vertex_count);
    geometry->indices.reserve(index_count);
    snapshot->storage_bytes_ = storage;
    ArrayReader arrays{state, memory, limits.max_array_bytes};
    for (std::size_t i = 0; i < draw.vertex_count; ++i) {
        auto& vertex = geometry->vertices[i];
        const auto* cursor = draw.vertices.data + i * layout.stride;
        for (unsigned m = 0; m < layout.matrix_indices.size(); ++m) {
            vertex.matrix_indices[m] = layout.matrix_indices[m] ? (*cursor++ & 63)
                : state.matrix_index(static_cast<std::uint8_t>(m));
        }
        if (indexed(layout.position)) {
            const auto sentinel = layout.position.mode == AttributeMode::index8 ? 255 : 65535;
            vertex.disabled = index(layout.position, cursor) == sentinel;
            if (vertex.disabled) continue;
        }
        if (!numbers(arrays, layout.position, 0, cursor, layout.byte_dequant, false, vertex.position.data()) ||
            !numbers(arrays, layout.normal, 1, cursor, layout.byte_dequant, true, vertex.normal.data())) {
            return failure(arrays.error, "GX indexed vertex data unavailable or read limit exceeded");
        }
        for (unsigned c = 0; c < layout.colors.size(); ++c) {
            const auto& a = layout.colors[c];
            if (a.mode == AttributeMode::none) continue;
            const auto* data = cursor;
            if (indexed(a)) data = arrays.read(2 + c, index(a, cursor), 0, color_size[a.format]).bytes;
            cursor += a.fifo_bytes;
            if (!data) return failure(arrays.error, "GX indexed color unavailable or read limit exceeded");
            color(data, a.format, vertex.colors[c]);
        }
        for (unsigned t = 0; t < layout.texcoords.size(); ++t) {
            if (!numbers(arrays, layout.texcoords[t], 4 + t, cursor, layout.byte_dequant, false,
                         vertex.texcoords[t].data())) {
                return failure(arrays.error, "GX indexed texture coordinate unavailable or read limit exceeded");
            }
        }
    }
    bool partial_disable = false;
    auto emit = [&](std::initializer_list<std::size_t> vertices) {
        const auto disabled = std::count_if(vertices.begin(), vertices.end(), [&](std::size_t i) {
            return geometry->vertices[i].disabled;
        });
        if (disabled == 0) {
            for (auto i : vertices) geometry->indices.push_back(static_cast<std::uint16_t>(i));
        } else if (static_cast<std::size_t>(disabled) != vertices.size()) {
            partial_disable = true;
        }
    };
    const std::size_t n = draw.vertex_count;
    switch (draw.primitive) {
    case 0x80:
        for (std::size_t i = 0; i < n; i += 4) { emit({i, i + 1, i + 2}); emit({i, i + 2, i + 3}); }
        break;
    case 0x90:
        for (std::size_t i = 0; i < n; i += 3) emit({i, i + 1, i + 2});
        break;
    case 0x98:
        for (std::size_t i = 2; i < n; ++i) {
            if (i % 2 == 0) emit({i - 2, i - 1, i});
            else emit({i - 2, i, i - 1});
        }
        break;
    case 0xa0:
        for (std::size_t i = 2; i < n; ++i) emit({0, i - 1, i});
        break;
    case 0xa8:
        for (std::size_t i = 0; i < n; i += 2) emit({i, i + 1});
        break;
    case 0xb0:
        for (std::size_t i = 1; i < n; ++i) emit({i - 1, i});
        break;
    case 0xb8:
        for (std::size_t i = 0; i < n; ++i) emit({i});
        break;
    }
    if (partial_disable) {
        return failure(VertexError::partially_disabled_primitive, "GX partially disabled primitive has undefined behavior");
    }
    snapshot->storage_bytes_=sizeof(VertexSnapshot)+sizeof(VertexSnapshot::Geometry)+
        geometry->vertices.capacity()*sizeof(ExpandedVertex)+geometry->indices.capacity()*sizeof(std::uint16_t);
    if(snapshot->storage_bytes_>limits.max_storage_bytes)
        return failure(VertexError::storage_limit,"GX vertex snapshot exceeds storage limit");
    snapshot->array_bytes_read_ = limits.max_array_bytes - arrays.remaining;
    std::shared_ptr<const VertexSnapshot> result(std::move(snapshot));
    if(cache)cache->impl_->insert(key,hash,draw,arrays,result);
    return {VertexError::none, {}, std::move(result)};
}

} // namespace starlet::gx
