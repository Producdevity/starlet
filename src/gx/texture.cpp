#include "starlet/gx/texture.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace starlet::gx {
namespace {
using Color = std::array<std::uint8_t, 4>;
struct Tile { unsigned width; unsigned height; unsigned bytes; };
bool tile_for(TextureFormat format, Tile& tile) {
    switch (format) {
    case TextureFormat::i4: case TextureFormat::ci4: case TextureFormat::cmpr: tile = {8,8,32}; return true;
    case TextureFormat::i8: case TextureFormat::ia4: case TextureFormat::ci8: tile = {8,4,32}; return true;
    case TextureFormat::ia8: case TextureFormat::rgb565: case TextureFormat::rgb5a3:
    case TextureFormat::ci14x2: tile = {4,4,32}; return true;
    case TextureFormat::rgba8: tile = {4,4,64}; return true;
    }
    return false;
}
bool indexed(TextureFormat f) {
    return f == TextureFormat::ci4 || f == TextureFormat::ci8 || f == TextureFormat::ci14x2;
}
bool power_of_two(unsigned n) { return (n & (n - 1)) == 0; }
std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>((unsigned{p[0]} << 8) | p[1]); }
std::uint8_t e4(unsigned n) { return static_cast<std::uint8_t>(n * 17); }
std::uint8_t e5(unsigned n) { return static_cast<std::uint8_t>((n << 3) | (n >> 2)); }
std::uint8_t e6(unsigned n) { return static_cast<std::uint8_t>((n << 2) | (n >> 4)); }
Color rgb565(unsigned n) { return {e5(n >> 11), e6((n >> 5) & 63), e5(n & 31), 255}; }
Color rgb5a3(unsigned n) {
    if (n & 0x8000) return {e5((n >> 10) & 31), e5((n >> 5) & 31), e5(n & 31), 255};
    const unsigned a = (n >> 12) & 7;
    return {e4((n >> 8) & 15), e4((n >> 4) & 15), e4(n & 15),
            static_cast<std::uint8_t>((a << 5) | (a << 2) | (a >> 1))};
}
Color ia8(const std::uint8_t* p) { return {p[1], p[1], p[1], p[0]}; }
unsigned palette_index(TextureFormat format, const std::uint8_t* tile, unsigned n) {
    if (format == TextureFormat::ci4) return (tile[n / 2] >> ((n & 1) ? 0 : 4)) & 15;
    if (format == TextureFormat::ci8) return tile[n];
    return be16(tile + n * 2) & 0x3fff;
}
Color palette_color(TexturePaletteFormat format, const std::uint8_t* p) {
    if (format == TexturePaletteFormat::ia8) return ia8(p);
    if (format == TexturePaletteFormat::rgb565) return rgb565(be16(p));
    return rgb5a3(be16(p));
}
Color cmpr(const std::uint8_t* tile, unsigned x, unsigned y, TextureCmprModel model) {
    const auto* block = tile + ((y / 4) * 2 + x / 4) * 8;
    const auto a = be16(block), b = be16(block + 2);
    const Color c0 = rgb565(a), c1 = rgb565(b);
    const unsigned selector = (block[4 + y % 4] >> (6 - 2 * (x % 4))) & 3;
    if (selector == 0) return c0;
    if (selector == 1) return c1;
    if (a <= b && selector == 3 && model == TextureCmprModel::s3tc_candidate_v1) return {0,0,0,0};
    Color out{0,0,0,std::uint8_t(a <= b && selector == 3 ? 0 : 255)};
    for (unsigned c = 0; c < 3; ++c) {
        if (a <= b) out[c] = static_cast<std::uint8_t>((unsigned{c0[c]} + c1[c]) / 2);
        else if (model == TextureCmprModel::gx_candidate_v1) {
            const unsigned weight = selector == 2 ? 5 : 3;
            out[c] = static_cast<std::uint8_t>((unsigned{c0[c]}*weight + unsigned{c1[c]}*(8-weight))/8);
        }
        else if (selector == 2) out[c] = static_cast<std::uint8_t>((2u * c0[c] + c1[c]) / 3);
        else out[c] = static_cast<std::uint8_t>((c0[c] + 2u * c1[c]) / 3);
    }
    return out;
}
Color direct_color(TextureFormat format, const std::uint8_t* tile, unsigned n, unsigned x, unsigned y, TextureCmprModel model) {
    switch (format) {
    case TextureFormat::i4: {
        const auto i = e4((tile[n / 2] >> ((n & 1) ? 0 : 4)) & 15);
        return {i,i,i,i};
    }
    case TextureFormat::i8: return {tile[n],tile[n],tile[n],tile[n]};
    case TextureFormat::ia4: {
        const auto i = e4(tile[n] & 15);
        return {i,i,i,e4(tile[n] >> 4)};
    }
    case TextureFormat::ia8: return ia8(tile + n * 2);
    case TextureFormat::rgb565: return rgb565(be16(tile + n * 2));
    case TextureFormat::rgb5a3: return rgb5a3(be16(tile + n * 2));
    case TextureFormat::rgba8: return {tile[n*2+1],tile[32+n*2],tile[33+n*2],tile[n*2]};
    case TextureFormat::cmpr: return cmpr(tile, x, y, model);
    default: return {}; // Indexed formats are handled before this dispatch.
    }
}
template<class F> void walk(const TextureLevel& level, Tile tile, const std::uint8_t* source, F visit) {
    std::size_t offset = level.source_offset;
    for (unsigned by = 0; by < level.height; by += tile.height) {
        for (unsigned bx = 0; bx < level.width; bx += tile.width, offset += tile.bytes) {
            for (unsigned y = 0; y < tile.height && by + y < level.height; ++y) {
                for (unsigned x = 0; x < tile.width && bx + x < level.width; ++x) {
                    visit(source + offset, y * tile.width + x, x, y,
                          (std::size_t{by + y} * level.width + bx + x) * 4);
                }
            }
        }
    }
}
TextureResult failure(TextureError code, const char* message) { return {code, message, {}}; }
bool add_budget(std::size_t& amount, std::size_t add, std::size_t limit) {
    if (amount > limit || add > limit - amount) return false;
    amount += add;
    return true;
}
}

struct TextureCache::Impl {
    struct Entry {
        TextureFormat format;
        TexturePaletteFormat palette_format;
        TextureCmprModel model;
        std::uint16_t width, height;
        std::vector<std::uint8_t> source;
        std::vector<TexturePaletteChunk> palette;
        std::shared_ptr<const std::vector<TextureLevel>> levels;
        std::size_t bytes;
        std::uint64_t used;
    };
    explicit Impl(TextureCacheLimits value): limits(value) {
        limits.entries=std::min(limits.entries,limits.bytes/sizeof(Entry));
        recent.fill(std::numeric_limits<std::size_t>::max());
        entries.reserve(limits.entries);
        if (entries.capacity()>limits.bytes/sizeof(Entry)) throw std::invalid_argument("texture cache metadata exceeds budget");
        stats.bytes=entries.capacity()*sizeof(Entry);
    }
    TextureCacheLimits limits;
    TextureCacheStats stats;
    std::vector<Entry> entries;
    std::array<std::size_t,8> recent;
    std::uint64_t sequence = 0;
    auto find(const TextureDescriptor& d, TextureCmprModel model, std::size_t count,
              ByteView source, const std::vector<TexturePaletteChunk>& palette) {
        const auto matches = [&](const Entry& entry) {
            if (entry.format != d.format || entry.palette_format != d.palette_format || entry.model != model ||
                entry.width != d.width || entry.height != d.height || entry.levels->size() != count ||
                entry.source.size() != source.size || entry.palette.size() != palette.size() ||
                !std::equal(entry.source.begin(), entry.source.end(), source.data)) return false;
            for (std::size_t i=0; i<palette.size(); ++i)
                if (entry.palette[i].first_index != palette[i].first_index || entry.palette[i].bytes != palette[i].bytes)
                    return false;
            return true;
        };
        auto& hint = recent[d.map];
        if (hint < entries.size() && matches(entries[hint]))
            return entries.begin() + static_cast<std::ptrdiff_t>(hint);
        auto found = std::find_if(entries.begin(), entries.end(), matches);
        if (found != entries.end()) hint = static_cast<std::size_t>(found - entries.begin());
        return found;
    }
    void insert(const TextureDescriptor& d, TextureCmprModel model, std::vector<std::uint8_t> source,
                const std::vector<TexturePaletteChunk>& palette, std::shared_ptr<const std::vector<TextureLevel>> levels) {
        std::size_t bytes = source.capacity() + palette.size()*sizeof(TexturePaletteChunk) +
                            levels->capacity()*sizeof(TextureLevel);
        for (const auto& level : *levels) bytes += level.rgba.capacity();
        if (!limits.entries || bytes > limits.bytes-entries.capacity()*sizeof(Entry)) return;
        while (!entries.empty() && (entries.size() >= limits.entries || stats.bytes > limits.bytes-bytes)) {
            auto oldest=std::min_element(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.used<b.used;});
            const auto removed = static_cast<std::size_t>(oldest - entries.begin());
            for (auto& hint : recent) {
                if (hint == removed) hint = std::numeric_limits<std::size_t>::max();
                else if (hint < entries.size() && hint > removed) --hint;
            }
            stats.bytes-=oldest->bytes;entries.erase(oldest);
        }
        entries.push_back({d.format,d.palette_format,model,d.width,d.height,std::move(source),palette,std::move(levels),bytes,++sequence});
        recent[d.map] = entries.size() - 1;
        stats.bytes+=bytes;stats.entries=entries.size();
    }
};
TextureCache::TextureCache(TextureCacheLimits limits):impl_(std::make_unique<Impl>(limits)) {}
TextureCache::~TextureCache() = default;
TextureCacheStats TextureCache::stats() const { return impl_->stats; }

TextureResult describe_texture(const State& state, std::uint8_t map, TextureCmprModel model,
                               TextureLimits limits, TextureDescriptor& d) {
    if (map >= 8) return failure(TextureError::invalid_map, "GX texture map exceeds seven");
    if (model != TextureCmprModel::reject_unverified && model != TextureCmprModel::s3tc_candidate_v1 &&
        model != TextureCmprModel::gx_candidate_v1)
        return failure(TextureError::unsupported_cmpr_model, "unknown CMPR arithmetic model");
    d = {};
    d.map = map;
    const unsigned base = (map < 4 ? 0x80 : 0xa0) + map % 4;
    for (unsigned i = 0; i < d.bp_words.size(); ++i)
        d.bp_words[i] = state.bp(static_cast<std::uint8_t>(base + i * 4));
    const auto m0 = d.bp_words[0], m1 = d.bp_words[1], image = d.bp_words[2];
    const unsigned s = m0 & 3, t = (m0 >> 2) & 3, filter = (m0 >> 5) & 7, aniso = (m0 >> 19) & 3;
    if (s == 3 || t == 3 || filter == 3 || filter == 7 || aniso == 3 ||
        (m0 & 0xc60000) || (m1 & 0xff0000) || (d.bp_words[3] & 0xe00000) ||
        (d.bp_words[4] & 0xe00000))
        return failure(TextureError::unsupported_state, "reserved sampler state or preloaded TMEM texture");
    d.sampler.wrap_s = static_cast<TextureWrap>(s);
    d.sampler.wrap_t = static_cast<TextureWrap>(t);
    d.sampler.min_filter = static_cast<TextureMinFilter>(filter);
    d.sampler.linear_magnification = (m0 & 16) != 0;
    d.sampler.edge_lod = (m0 & 256) == 0;
    const int bias = (m0 >> 9) & 255;
    d.sampler.lod_bias_thirtyseconds = static_cast<std::int16_t>(bias < 128 ? bias : bias - 256);
    d.sampler.bias_clamp = (m0 & 0x200000) != 0;
    d.sampler.anisotropy = static_cast<std::uint8_t>(1u << aniso);
    d.sampler.min_lod_sixteenths = static_cast<std::uint8_t>(m1);
    d.sampler.max_lod_sixteenths = static_cast<std::uint8_t>(m1 >> 8);
    if (d.sampler.min_lod_sixteenths > d.sampler.max_lod_sixteenths ||
        d.sampler.max_lod_sixteenths > 160 || (aniso != 0 && !d.sampler.edge_lod))
        return failure(TextureError::unsupported_state, "invalid LOD range or diagonal anisotropy");
    d.width = static_cast<std::uint16_t>((image & 1023) + 1);
    d.height = static_cast<std::uint16_t>(((image >> 10) & 1023) + 1);
    const auto& extent = state.texture_extent(map);
    if (extent.width && ((extent.width - 1u) & 1023u) == (image & 1023u) && ((extent.height - 1u) & 1023u) == ((image >> 10) & 1023u)) {
        d.width = extent.width;
        d.height = extent.height;
    }
    d.format = static_cast<TextureFormat>(image >> 20);
    d.address = d.bp_words[5] << 5;
    Tile tile{};
    if (!tile_for(d.format, tile)) return failure(TextureError::unsupported_format, "reserved GX texture format");
    if (d.format == TextureFormat::cmpr && model == TextureCmprModel::reject_unverified)
        return failure(TextureError::unsupported_cmpr_model, "CMPR needs explicit unverified arithmetic model");
    if (d.width > limits.max_dimension || d.height > limits.max_dimension)
        return failure(TextureError::dimension_limit, "GX texture dimension limit exceeded");
    const bool mipmapped = filter != 0 && filter != 4;
    if (mipmapped && (!power_of_two(d.width) || !power_of_two(d.height)))
        return failure(TextureError::unsupported_state, "GX mipmaps require power-of-two dimensions");
    if (indexed(d.format)) {
        if ((d.bp_words[6] & 0xfff000) || ((d.bp_words[6] >> 10) & 3) == 3)
            return failure(TextureError::unsupported_state, "reserved TLUT configuration");
        d.palette_format = static_cast<TexturePaletteFormat>((d.bp_words[6] >> 10) & 3);
        d.palette_offset = (d.bp_words[6] & 1023) * std::size_t{512};
    }
    return {};
}

std::size_t texture_source_bytes(const TextureDescriptor& d, unsigned levels) {
    Tile tile{};
    if (!tile_for(d.format, tile)) return 0;
    std::size_t bytes = 0;
    unsigned width = d.width, height = d.height;
    for (unsigned lod = 0; lod < levels; ++lod) {
        bytes += std::size_t{(width + tile.width - 1) / tile.width} * ((height + tile.height - 1) / tile.height) * tile.bytes;
        width = std::max(1u, width / 2); height = std::max(1u, height / 2);
    }
    return bytes;
}

TextureResult capture_texture(const State& state, std::uint8_t map, MemoryReader& memory,
                              TextureCmprModel model, TextureLimits limits, TextureCache* cache) {
    TextureDescriptor d;
    auto described = describe_texture(state, map, model, limits, d);
    if (!described) return described;
    Tile tile{};
    tile_for(d.format, tile);
    const auto filter = static_cast<unsigned>(d.sampler.min_filter);
    const bool mipmapped = filter != 0 && filter != 4;
    std::array<TextureLevel, 11> layout;
    std::size_t count = 0, source_bytes = 0, rgba_bytes = 0;
    unsigned width = d.width, height = d.height;
    const unsigned last = mipmapped ? (d.sampler.max_lod_sixteenths + 15u) / 16u : 0;
    for (unsigned lod = 0; lod <= last; ++lod) {
        if (count >= limits.max_levels) return failure(TextureError::level_limit, "GX texture mip level limit exceeded");
        auto& level = layout[count++];
        level.width = static_cast<std::uint16_t>(width);
        level.height = static_cast<std::uint16_t>(height);
        level.source_offset = source_bytes;
        level.source_size = std::size_t{(width + tile.width - 1) / tile.width} *
                            ((height + tile.height - 1) / tile.height) * tile.bytes;
        if (!add_budget(source_bytes, level.source_size, limits.max_source_bytes))
            return failure(TextureError::source_limit, "GX texture source byte limit exceeded");
        rgba_bytes += std::size_t{width} * height * 4;
        if (width == 1 && height == 1) break;
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
    if (source_bytes > std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 1 - d.address)
        return failure(TextureError::memory_read, "GX texture address range overflows");
    std::size_t peak = sizeof(TextureSnapshot);
    if (!add_budget(peak, count * sizeof(TextureLevel), limits.max_storage_bytes) ||
        !add_budget(peak, rgba_bytes, limits.max_storage_bytes) ||
        !add_budget(peak, source_bytes, limits.max_storage_bytes))
        return failure(TextureError::storage_limit, "GX texture storage limit exceeded");
    std::vector<std::uint8_t> owned_source;
    auto source = memory.view(d.address, source_bytes);
    if (!source.data || source.size != source_bytes) {
        owned_source.resize(source_bytes);
        if (!memory.copy(d.address, owned_source.data(), owned_source.size()))
            return failure(TextureError::memory_read, "GX texture source read failed");
        source = {owned_source.data(), owned_source.size()};
    }
    std::array<std::uint16_t, 1024> chunk_slots;
    std::size_t chunk_count = 0;
    if (indexed(d.format)) {
        chunk_slots.fill(0);
        for (std::size_t i = 0; i < count; ++i) {
            walk(layout[i], tile, source.data, [&](const std::uint8_t* p, unsigned n, unsigned, unsigned, std::size_t) {
                chunk_slots[palette_index(d.format, p, n) / 16] = 1;
            });
        }
        for (const auto used : chunk_slots) chunk_count += used;
        if (!add_budget(peak, chunk_count * sizeof(TexturePaletteChunk), limits.max_storage_bytes))
            return failure(TextureError::storage_limit, "GX TLUT storage limit exceeded");
    }
    std::shared_ptr<TextureSnapshot> out(new TextureSnapshot);
    out->descriptor_ = d;
    out->state_revision_ = state.revision();
    out->source_bytes_read_ = source_bytes;
    out->storage_bytes_ = peak - source_bytes;
    out->cmpr_model_ = d.format == TextureFormat::cmpr ? model : TextureCmprModel::reject_unverified;
    out->palette_.resize(chunk_count);
    if (indexed(d.format)) {
        std::size_t slot = 0;
        for (std::size_t i = 0; i < chunk_slots.size(); ++i) {
            if (!chunk_slots[i]) continue;
            auto& chunk = out->palette_[slot];
            chunk_slots[i] = static_cast<std::uint16_t>(slot++);
            chunk.first_index = static_cast<std::uint16_t>(i * 16);
            const auto offset = d.palette_offset + i * 32;
            if (!state.read_palette(offset, chunk.bytes.data(), chunk.bytes.size()) ||
                !state.palette_generation(offset, chunk.bytes.size(), chunk.generation))
                return failure(TextureError::palette_read, "GX texture references unloaded or out-of-range TLUT entries");
        }
    }
    if (cache) {
        auto& c=*cache->impl_;
        auto found=c.find(d,out->cmpr_model_,count,source,out->palette_);
        if (found!=c.entries.end()) {
            ++c.stats.hits;found->used=++c.sequence;out->levels_=found->levels;
            return {TextureError::none,{},std::move(out)};
        }
        ++c.stats.misses;
    }
    auto levels=std::make_shared<std::vector<TextureLevel>>(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto& level = (*levels)[i];
        level = std::move(layout[i]);
        level.rgba.resize(std::size_t{level.width} * level.height * 4);
        walk(level, tile, source.data, [&](const std::uint8_t* p, unsigned n, unsigned x, unsigned y, std::size_t dest) {
            Color color;
            if (indexed(d.format)) {
                const unsigned index = palette_index(d.format, p, n);
                const auto& chunk = out->palette_[chunk_slots[index / 16]];
                color = palette_color(d.palette_format, chunk.bytes.data() + (index % 16) * 2);
            } else color = direct_color(d.format, p, n, x, y, model);
            std::copy(color.begin(), color.end(), level.rgba.begin() + static_cast<std::ptrdiff_t>(dest));
        });
    }
    out->levels_=levels;
    if (cache) {
        if (owned_source.empty()) owned_source.assign(source.data, source.data + source.size);
        cache->impl_->insert(d,out->cmpr_model_,std::move(owned_source),out->palette_,levels);
    }
    return {TextureError::none, {}, std::move(out)};
}
} // namespace starlet::gx
