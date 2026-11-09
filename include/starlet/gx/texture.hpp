#pragma once

#include "starlet/gx/decoder.hpp"

namespace starlet::gx {

enum class TextureFormat : std::uint8_t {
    i4 = 0, i8 = 1, ia4 = 2, ia8 = 3, rgb565 = 4, rgb5a3 = 5, rgba8 = 6,
    ci4 = 8, ci8 = 9, ci14x2 = 10, cmpr = 14
};
enum class TextureWrap : std::uint8_t { clamp, repeat, mirror };
enum class TextureMinFilter : std::uint8_t {
    nearest = 0, nearest_mip_nearest = 1, nearest_mip_linear = 2,
    linear = 4, linear_mip_nearest = 5, linear_mip_linear = 6
};
enum class TexturePaletteFormat : std::uint8_t { ia8, rgb565, rgb5a3 };

struct TextureSampler {
    TextureWrap wrap_s = TextureWrap::clamp;
    TextureWrap wrap_t = TextureWrap::clamp;
    TextureMinFilter min_filter = TextureMinFilter::nearest;
    bool linear_magnification = false;
    std::uint8_t min_lod_sixteenths = 0;
    std::uint8_t max_lod_sixteenths = 0;
    std::int16_t lod_bias_thirtyseconds = 0;
    bool edge_lod = true;
    bool bias_clamp = false;
    std::uint8_t anisotropy = 1;
};

// The GX manuals specify CMPR storage, but not exact interpolation rounding or
// transparent RGB. Explicit candidates use expanded RGB565 with integer floor:
// S3TC uses thirds/transparent black; GX uses eighths/transparent average RGB.
// Neither is proof of original-hardware precision.
enum class TextureCmprModel : std::uint8_t { reject_unverified, s3tc_candidate_v1, gx_candidate_v1 };

struct TextureDescriptor {
    TextureFormat format = TextureFormat::i4;
    std::uint16_t width = 1;
    std::uint16_t height = 1;
    std::uint32_t address = 0;
    std::uint8_t map = 0;
    TextureSampler sampler;
    TexturePaletteFormat palette_format = TexturePaletteFormat::ia8;
    std::size_t palette_offset = 0; // Relative to TMEM 0x80000; used only for CI.
    // MODE0, MODE1, IMAGE0..3, TLUT. Cache layout is preserved, not emulated.
    std::array<std::uint32_t, 7> bp_words{};
};
struct TextureLevel {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::size_t source_offset = 0;
    std::size_t source_size = 0;
    std::vector<std::uint8_t> rgba; // Row-major R,G,B,A; no tile padding or flip.
};
struct TexturePaletteChunk {
    std::uint16_t first_index = 0;
    std::array<std::uint8_t, 32> bytes{};
    // Canonical State generation of the containing 512-byte palette block.
    std::uint64_t generation = 0;
};
struct TextureLimits {
    // GX registers hold at most 1024; a host TextureExtent may go further.
    std::size_t max_dimension = 8192;
    std::size_t max_levels = 11;
    std::size_t max_source_bytes = 8 * 1024 * 1024;
    // Peak object/vector payload including temporary source bytes; excludes
    // allocator bookkeeping, bounded stack scratch, and caller-owned data.
    std::size_t max_storage_bytes = 16 * 1024 * 1024;
};
enum class TextureError {
    none, invalid_map, unsupported_state, unsupported_format, unsupported_cmpr_model,
    dimension_limit, level_limit, source_limit, storage_limit, memory_read, palette_read
};
class TextureSnapshot;
struct TextureResult;
struct TextureCacheLimits { std::size_t bytes = 32 * 1024 * 1024, entries = 128; };
struct TextureCacheStats { std::uint64_t hits = 0, misses = 0; std::size_t bytes = 0, entries = 0; };
// Owner-thread cache of immutable decoded content. Exact source/TLUT bytes form
// the key; addresses and sampler state do not. Evicted data can remain alive in
// caller-owned packets, whose retention is bounded separately.
class TextureCache {
public:
    explicit TextureCache(TextureCacheLimits = {});
    ~TextureCache();
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;
    TextureCacheStats stats() const;
private:
    friend TextureResult capture_texture(const State&, std::uint8_t, MemoryReader&, TextureCmprModel, TextureLimits, TextureCache*);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct TextureResult {
    TextureError error = TextureError::none;
    std::string message;
    std::shared_ptr<const TextureSnapshot> snapshot;
    explicit operator bool() const { return error == TextureError::none; }
};
class TextureSnapshot {
public:
    const TextureDescriptor& descriptor() const { return descriptor_; }
    const std::vector<TextureLevel>& levels() const { return *levels_; }
    const std::vector<TexturePaletteChunk>& palette() const { return palette_; }
    TextureCmprModel cmpr_model() const { return cmpr_model_; }
    std::uint64_t state_revision() const { return state_revision_; }
    std::size_t source_bytes_read() const { return source_bytes_read_; }
    std::size_t storage_bytes() const { return storage_bytes_; }
    std::size_t peak_storage_bytes() const { return storage_bytes_ + source_bytes_read_; }
private:
    friend TextureResult capture_texture(const State&, std::uint8_t, MemoryReader&, TextureCmprModel, TextureLimits, TextureCache*);
    TextureSnapshot() = default;
    TextureDescriptor descriptor_;
    std::shared_ptr<const std::vector<TextureLevel>> levels_;
    std::vector<TexturePaletteChunk> palette_;
    TextureCmprModel cmpr_model_ = TextureCmprModel::reject_unverified;
    std::uint64_t state_revision_ = 0;
    std::size_t source_bytes_read_ = 0;
    std::size_t storage_bytes_ = 0;
};

// Synchronous draw capture: state and memory must remain stable until return.
// Reads original tiled big-endian GX bytes through address tokens, never native
// pointers. Owns all visible mip texels and referenced 32-byte TLUT chunks.
// Captures levels 0..ceil(max LOD), capped at 1x1; non-mip filters need only LOD0.
// Failure publishes nothing; allocation/provider exceptions propagate. There is
// an optional content cache. Callers bound retained snapshots and GPU resources.
// TEV routing, SU coordinate scales, texgen/projection, filtering precision and
// GX cache-invalidation/coherence are external. A GLES upload must preserve
// each supplied level and sampler field or explicitly reject unsupported ones.
// capture_texture's register decoding and rejection rules, without reading texels.
TextureResult describe_texture(const State&, std::uint8_t map, TextureCmprModel, TextureLimits, TextureDescriptor&);
// Tiled source bytes of the first levels mip levels, as capture_texture reads them.
std::size_t texture_source_bytes(const TextureDescriptor&, unsigned levels);
TextureResult capture_texture(const State&, std::uint8_t map, MemoryReader&,
                              TextureCmprModel = TextureCmprModel::reject_unverified,
                              TextureLimits = {}, TextureCache* = nullptr);

} // namespace starlet::gx
