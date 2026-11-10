#include "starlet/gx/gles/direct.hpp"
#include "starlet/gx/gles/program_builder.hpp"
#include "starlet/gx/gles/program_cache.hpp"
#include "starlet/gx/gles/program_list.hpp"
#include "starlet/gx/line_point.hpp"
#include "starlet/gx/vertex.hpp"

#include <GLES3/gl32.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace starlet::gx {
namespace {
// A block's stamp: its first eight bytes, unique per copy and block.
std::uint64_t stamp_word(std::uint64_t stamp, std::size_t block) { return stamp * 0x9e3779b97f4a7c15ull + block; }
// Up to 64 blocks spread over a copy, its first and last included: a game reusing the
// memory overwrites far more than that, and stamping every block of full-screen copies
// costs a cache miss per block on small CPUs.
template<class F> bool sampled_blocks(std::size_t blocks, F f) {
    const std::size_t step = std::max<std::size_t>(1, blocks / 63);
    for (std::size_t block = 0; block < blocks; block += step)
        if (!f(block)) return false;
    return !blocks || f(blocks - 1);
}
} // namespace
void GlesResidentCopy::write_stamps() const {
    sampled_blocks(bytes / 32, [this](std::size_t block) {
        const auto word = stamp_word(stamp, block);
        std::memcpy(destination + block * 32, &word, sizeof word);
        return true;
    });
}
bool GlesResidentCopy::stamped(std::size_t block) const {
    const auto word = stamp_word(stamp, block);
    return std::memcmp(destination + block * 32, &word, sizeof word) == 0;
}
bool GlesResidentCopy::intact() const {
    return !stamp || sampled_blocks(bytes / 32, [this](std::size_t block) { return stamped(block); });
}
namespace {
// The array still holds its numbers in the encoding a span was converted from.
bool same_encoding(const MemoryArrayView& view, std::uint8_t native_unit, bool host_order) {
    return view.host_order == host_order && (host_order || view.native_unit == native_unit);
}
} // namespace
namespace {
// Leaves resized elements uninitialized: uniform blocks are written whole after each resize.
template <class T> struct UninitializedAllocator : std::allocator<T> {
    template <class U> struct rebind { using other = UninitializedAllocator<U>; };
    UninitializedAllocator() = default;
    template <class U> UninitializedAllocator(const UninitializedAllocator<U>&) noexcept {}
    template <class U> void construct(U* p) noexcept { ::new (static_cast<void*>(p)) U; }
    template <class U, class... A> void construct(U* p, A&&... a) { ::new (static_cast<void*>(p)) U(std::forward<A>(a)...); }
};
using UniformBytes = std::vector<std::uint8_t, UninitializedAllocator<std::uint8_t>>;
void gl_check(const char* operation) {
    const auto error = glGetError();
    if (error != GL_NO_ERROR) throw std::runtime_error(std::string(operation) + ": GLES error " + std::to_string(error));
}
std::uint64_t hash_words(const std::vector<std::uint32_t>& words) {
    std::uint64_t h = 14695981039346656037ull;
    for (auto w : words) { h ^= w; h *= 1099511628211ull; }
    return h;
}
// Four independent multiply-rotate lanes; collisions only matter against the
// same key's previous content, so a 64-bit non-cryptographic mix suffices.
std::uint64_t hash_bytes(const std::uint8_t* p, std::size_t n) {
    constexpr std::uint64_t k = 0x9e3779b97f4a7c15ull;
    std::uint64_t a = n, b = k, c = ~k, d = n * k;
    auto rotate = [](std::uint64_t x, unsigned r) { return (x << r) | (x >> (64 - r)); };
    std::size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        std::uint64_t w[4]; std::memcpy(w, p + i, 32);
        a = rotate(a ^ w[0], 29) * k; b = rotate(b ^ w[1], 31) * k;
        c = rotate(c ^ w[2], 27) * k; d = rotate(d ^ w[3], 33) * k;
    }
    for (; i < n; ++i) a = rotate(a ^ p[i], 7) * k;
    return rotate(a, 1) ^ rotate(b, 7) ^ rotate(c, 13) ^ rotate(d, 19) ^ (a * b) ^ (c + d);
}
template<class T> struct Cached {
    GLint location = -1;
    T value{};
    bool set = false;
    bool update(const T& next) {
        if (location < 0 || (set && !std::memcmp(value.data(), next.data(), sizeof(T)))) return false;
        value = next; set = true; return true;
    }
};
GLenum wrap(TextureWrap value) {
    switch (value) {
    case TextureWrap::clamp: return GL_CLAMP_TO_EDGE;
    case TextureWrap::repeat: return GL_REPEAT;
    case TextureWrap::mirror: return GL_MIRRORED_REPEAT;
    }
    throw std::runtime_error("invalid admitted texture wrap");
}
GLenum min_filter(TextureMinFilter filter) {
    switch (filter) {
    case TextureMinFilter::nearest: return GL_NEAREST;
    case TextureMinFilter::nearest_mip_nearest: return GL_NEAREST_MIPMAP_NEAREST;
    case TextureMinFilter::nearest_mip_linear: return GL_NEAREST_MIPMAP_LINEAR;
    case TextureMinFilter::linear: return GL_LINEAR;
    case TextureMinFilter::linear_mip_nearest: return GL_LINEAR_MIPMAP_NEAREST;
    case TextureMinFilter::linear_mip_linear: return GL_LINEAR_MIPMAP_LINEAR;
    }
    throw std::runtime_error("invalid texture minification filter");
}
GLenum factor(unsigned value, bool source) {
    const GLenum factors[] = {GL_ZERO, GL_ONE, static_cast<GLenum>(source ? GL_DST_COLOR : GL_SRC_COLOR),
        static_cast<GLenum>(source ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR), GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
        GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    if (value >= 8) throw std::runtime_error("invalid admitted blend factor");
    return factors[value];
}
// Byte-identical vertices yield identical attributes, so every triangle has zero area.
bool zero_area(const Draw& draw) {
    const auto n = draw.vertex_count;
    const bool valid = (draw.primitive == 0x80 && n % 4 == 0) || (draw.primitive == 0x90 && n % 3 == 0) ||
                       ((draw.primitive == 0x98 || draw.primitive == 0xa0) && n >= 3);
    if (!valid || n < 3 || !draw.layout.stride || draw.vertices.size != draw.layout.stride * n) return false;
    for (std::size_t i = 1; i < n; ++i)
        if (std::memcmp(draw.vertices.data, draw.vertices.data + i * draw.layout.stride, draw.layout.stride)) return false;
    return true;
}
// Byte-identical records shade to identical vertices: keeping one of each, in first-use
// order, lets the GPU reuse a shaded vertex wherever strips and fans repeat it.
void merge_identical_vertices(std::vector<std::uint8_t>& vertices, std::vector<std::uint32_t>& indices, std::size_t stride) {
    const std::size_t count = vertices.size() / stride;
    if (count < 2) return;
    std::vector<std::uint32_t> remap(count);
    std::unordered_multimap<std::uint64_t, std::uint32_t> seen;
    seen.reserve(count);
    std::size_t kept = 0;
    for (std::size_t v = 0; v < count; ++v) {
        const auto* record = vertices.data() + v * stride;
        const auto hash = hash_bytes(record, stride);
        std::uint32_t found = ~0u;
        const auto range = seen.equal_range(hash);
        for (auto it = range.first; it != range.second; ++it)
            if (!std::memcmp(vertices.data() + std::size_t{it->second} * stride, record, stride)) { found = it->second; break; }
        if (found == ~0u) {
            found = static_cast<std::uint32_t>(kept);
            if (kept != v) std::memcpy(vertices.data() + kept * stride, record, stride);
            seen.emplace(hash, found);
            ++kept;
        }
        remap[v] = found;
    }
    if (kept == count) return;
    vertices.resize(kept * stride);
    for (auto& i : indices) i = remap[i];
}
std::size_t palette_bytes(TextureFormat format) {
    return format == TextureFormat::ci4 ? 32 : format == TextureFormat::ci8 ? 512 : format == TextureFormat::ci14x2 ? 32768 : 0;
}
}

struct GlesDirect::Impl {
    struct Program {
        std::vector<std::uint32_t> words;
        std::uint64_t hash = 0;
        std::shared_ptr<const DirectProgram> plan;
        GLuint name = 0;
        std::array<Cached<TevSignedColor>, 4> registers{};
        std::array<Cached<TevColor>, 4> konst{};
        std::array<Cached<std::array<float, 2>>, 16> scales{}, image_sizes{};
        std::array<Cached<std::array<float, 4>>, 4> indirect_scales{};
        std::array<Cached<std::array<std::int32_t, 4>>, 6> indirect_rows{};
        std::array<Cached<std::array<float, 4>>, 8> sampler_lod{};
        Cached<std::array<std::uint8_t, 2>> alpha;
        Cached<std::array<std::uint32_t, 4>> pixel;
        std::array<Cached<std::array<float, 1>>, 8> bias{};
        Cached<std::array<float, 3>> constant_alpha;
        Cached<std::array<float, 4>> fog;
        Cached<std::array<float, 3>> fog_color;
        Cached<std::array<float, 1>> fog_center;
        Cached<std::array<float, 10>> fog_table;
        Cached<std::array<std::uint32_t, 1>> z_bias;
        std::uint64_t used = 0, segment = 0;
        // Small draws of a batchable program use its batched variant; see draw_batched.
        bool batchable = false;
        Program* batched = nullptr;
        // Resident draws that differ only in their position and normal matrix share one
        // instanced draw of this variant; see join_instances.
        bool instanceable = false;
        Program* instanced = nullptr;
        // GL thread: handed to the builder; the segment that last found it unbuilt; the driver
        // rejected it.
        bool requested = false, rejected = false;
        std::uint64_t unbuilt = 0;
        std::chrono::steady_clock::time_point requested_at{};
        // Recording thread: the segment that last queued it for warm-up.
        std::uint64_t warmed = 0;
    };
    struct TextureKey {
        std::uint32_t address = 0, image = 0, levels = 0, palette = 0;
        std::uint64_t palette_hash = 0;
        TextureCmprModel model = TextureCmprModel::reject_unverified;
        bool operator==(const TextureKey& o) const {
            return address == o.address && image == o.image && levels == o.levels && palette == o.palette &&
                   palette_hash == o.palette_hash && model == o.model;
        }
    };
    struct Texture {
        TextureKey key;
        std::uint64_t key_hash = 0, content = 0, validated = 0, used = 0, segment = 0;
        GLuint name = 0;
        std::size_t bytes = 0, source_bytes = 0;
        const std::uint8_t* pointer = nullptr;
        // Source write generation at the last content check; see Geometry for the policy.
        std::uint64_t generation = 0;
        bool volatile_data = false;
    };
    struct Run { StreamFormat format; std::uint32_t base = 0, vertices = 0; };
    // Converted indexed geometry resident in its own GL buffers; only the FIFO
    // bytes and source-span hashes stay on the CPU for validation.
    struct Geometry {
        std::uint64_t hash = 0;
        std::vector<std::uint32_t> key;
        std::vector<std::uint8_t> fifo;
        struct Span { unsigned array; std::uint32_t base; std::size_t first, end; std::uint8_t native_unit; bool host_order;
                      std::vector<std::uint8_t> bytes; std::uint64_t generation; };
        std::vector<Span> spans;
        StreamFormat format;
        // Compared in full every frame once a source changed without a reported write.
        bool volatile_data = false;
        std::uint8_t phase = 0; // Frame offset of the entry's periodic full comparison.
        std::uint32_t vertex_count = 0, index_count = 0;
        bool short_indices = false;
        // Loose entries own buffer and keep data for later packing; packed ones live in an arena.
        GLuint buffer = 0;
        std::int32_t arena = -1, base_vertex = 0;
        std::uint32_t index_offset = 0, payload = 0;
        std::vector<std::uint8_t> data;
        std::uint64_t segment = 0, inserted = 0;
        std::size_t bytes = 0;
        // Display-list payload identity: unchanged while its source reports no write.
        const void* fifo_pointer = nullptr; // The payload's source key (MemoryReader::source_identity).
        std::uint64_t fifo_generation = 0;
        // Object-space position bounds (min xyz, max xyz) of single-matrix geometry.
        bool boxed = false;
        std::array<float, 6> box{};
        // Large meshes also keep bounds per run of consecutive triangles, so draws
        // submit only the runs inside the clip volume.
        // Indices [first, first + count) referencing vertices [low, high].
        struct Cluster { std::uint32_t first, count; std::array<float, 6> box; std::uint32_t low, high; };
        std::vector<Cluster> clusters;
    };
    // Mali drivers pay per draw for every live buffer object, and writing into a
    // buffer that submitted draws use makes them copy it whole. New geometry gets
    // its own buffer; settled entries are packed into a fresh exact-size arena.
    // Packed entries share one vertex array per arena and stream format and are
    // addressed by base vertex, keeping the number of driver objects small.
    struct Arena { GLuint buffer = 0; std::size_t capacity = 0, live = 0; };
    std::vector<Arena> arenas;
    std::vector<Geometry*> loose;
    std::size_t loose_bytes = 0;
    // Draw identities whose sources changed after caching; they are streamed.
    std::unordered_map<std::uint64_t, std::uint8_t> unstable_geometry;
    std::list<Geometry> geometries, retired_geometries;
    std::unordered_set<std::uint32_t> volatile_bases;
    std::vector<std::uint8_t> geometry_vertices;
    std::vector<std::uint32_t> geometry_indices;
    std::unordered_multimap<std::uint64_t, std::list<Geometry>::iterator> geometry_index;
    std::unordered_multimap<const void*, std::list<Geometry>::iterator> geometry_pointers;
    MemoryReader::SourceIdentity source; // The current draw's payload source (find_geometry_by_pointer).
    std::vector<std::uint32_t> geometry_key;
    StreamArraySpans spans;
    // Resident geometry as drawn: copied at record time, since packing moves entries
    // while earlier segments may still be executing.
    struct GeometryDraw {
        GLuint buffer = 0;
        StreamFormat format;
        std::int32_t base_vertex = 0;
        std::uint32_t index_offset = 0;
        bool short_indices = false;
    };
    struct Record {
        Program* program;
        bool resident; // Draws geometry instead of the segment run.
        GeometryDraw geometry;
        std::uint32_t state, uniform, run, first, count, low, high;
        std::array<GLuint, 8> textures, samplers;
        std::array<std::uint32_t, 3> latches;
        // Instanced records: instance count and the offset of their rows in the segment's uniforms.
        std::uint32_t instances = 0, instance_offset = 0;
    };
    // Everything one flush hands to the GL thread: GL work recorded since the last
    // flush runs first, then the draws, then deletions of objects they used.
    // Sources reported unchanged are still compared in full every 64th frame, staggered
    // per entry, against writes that bypass the report.
    static constexpr unsigned full_check_period = 63;
    struct Segment {
        std::vector<std::function<void()>> before, after;
        std::vector<Program*> warm;
        std::vector<Record> records;
        std::vector<DirectDrawState> states;
        std::vector<Run> runs;
        std::vector<std::uint8_t> vertices;
        UniformBytes uniforms;
        std::vector<std::uint32_t> indices;
    };
    std::vector<std::function<void()>> pending, retiring;
    std::vector<Program*> warming;
    // Executed segments return here with their storage kept for the next flush.
    std::mutex spare_mutex;
    std::vector<std::shared_ptr<Segment>> spare_segments;
    // GL names generated on the GL thread ahead of use; recording takes them so
    // records can name objects whose creation is still queued.
    struct NamePool {
        std::mutex mutex;
        std::vector<GLuint> textures, buffers, samplers, framebuffers;
        bool refilling = false;
    } names;
    enum class Kind { texture, buffer, sampler, framebuffer };

    GlesDirectLimits limits;
    FramebufferFetch fetch;
    Target target;
    GlesDirectStats stats;
    std::vector<std::unique_ptr<Program>> programs;
    // GL thread.
    std::unique_ptr<GlesProgramCache> program_cache;
    // Declared after the cache it fills, so it stops first.
    std::unique_ptr<GlesProgramBuilder> builder;
    std::unique_ptr<ProgramList> program_list; // GL thread.
    std::atomic<std::uint64_t> link_us{0}, unready_draws{0};
    std::array<std::atomic<std::uint64_t>, 5> program_waits{};
    std::atomic<std::uint64_t> program_wait_max_ms{0}, fallback_links{0}, fallback_link_max_ms{0};
    std::chrono::steady_clock::time_point last_fallback{}; // GL thread: end of its last link of an unready program.
    std::uint64_t gl_segments = 0, rejected_programs = 0; // GL thread.
    std::unordered_multimap<std::uint64_t, Program*> program_index;
    std::vector<std::unique_ptr<Texture>> textures;
    std::unordered_multimap<std::uint64_t, Texture*> texture_index;
    std::vector<std::pair<std::uint32_t, GLuint>> samplers;
    std::map<std::uint32_t, GlesResidentCopy> copies;
    std::uint64_t stamps = 0;
    // Copy targets are reused only once the GPU has retired their last frame of use:
    // rendering into a texture that pending draws still sample makes the driver wait.
    struct SpareTarget { int width, height; GLuint texture, framebuffer; std::uint64_t frame; };
    std::vector<SpareTarget> spare_targets;
    std::vector<std::uint8_t> vertices, scratch;
    UniformBytes uniforms;
    TransformUniforms transform_block;
    std::vector<std::uint32_t> indices, words;
    std::vector<Record> records;
    std::vector<DirectDrawState> states;
    std::vector<Run> runs;
    std::uint64_t segment = 1, frame = 1, sequence = 0;
    // Keyed by the BP revision that ignores TEV colour writes, which most draws make, and
    // the CP revision that ignores array bindings, which most draws change.
    struct { bool valid = false; std::uint64_t bp = 0, cp = 0, xf = 0; std::uint32_t registers = 0; std::uint8_t vat = 0;
             Program* program = nullptr; std::uint8_t latch = 0; } last_program;
    struct { bool valid = false; std::uint64_t bp = 0; std::uint32_t word = 0; } last_registers;
    struct { bool valid = false; std::uint64_t bp = 0, xf = 0, segment = 0; const Program* program = nullptr; std::uint32_t index = 0; } last_state;
    struct { bool valid = false; std::uint64_t memory = 0, registers = 0, segment = 0, latch = 0; std::uint32_t offset = 0; } last_uniform;
    // The XF's latched normal, binormal and tangent: components held (0, 3 or 9), values and
    // a revision that changes with them.
    std::uint8_t latch_components = 0;
    std::array<float, 9> latch{};
    std::uint64_t latch_revision = 0;
    struct { bool valid = false; std::uint64_t bp = 0, segment = 0, frame = 0; const Program* program = nullptr;
             std::array<GLuint, 8> textures{}, samplers{}; std::array<std::uint64_t, 8> palettes{}; } last_textures;
    // Palette contents a CI texture reads, as a generation; 0 for other formats.
    static std::uint64_t palette_generation(const State& state, const TextureDescriptor& d) {
        const auto bytes = palette_bytes(d.format);
        std::uint64_t generation = 0;
        if (bytes && !state.palette_generation(d.palette_offset, bytes, generation)) return ~std::uint64_t{0};
        return generation;
    }
    // BP values at the last state plan. Games rewrite unchanged registers between draws:
    // equal values keep the plan and the textures it resolved (CI palettes are checked
    // by generation, since a TLUT load changes them without changing a register).
    std::array<std::uint32_t, 256> planned_bp{};
    bool same_bp_values(const State& state) const {
        for (unsigned a = 0; a < 256; ++a) if (state.bp(static_cast<std::uint8_t>(a)) != planned_bp[a]) return false;
        return true;
    }
    bool same_palettes(const State& state, const Program& p) const {
        const auto& planned = states[last_state.index];
        for (unsigned map = 0; map < 8; ++map) if (p.plan->maps & (1u << map))
            if (palette_generation(state, planned.textures[map]) != last_textures.palettes[map]) return false;
        return true;
    }
    // Batched draws share state, textures and uniform block while only TEV color
    // registers and their own position matrix change between them.
    struct { bool valid = false; std::uint64_t bp = 0, xf = 0, segment = 0, frame = 0; const Program* program = nullptr;
             std::uint32_t state = 0; std::array<GLuint, 8> textures{}, samplers{}; bool expanded = false; } batch;
    struct { bool valid = false; std::uint64_t memory = 0, registers = 0, segment = 0; std::uint32_t offset = 0; unsigned pn = 0; } batch_uniform;
    TransformUniforms batch_block;
    std::vector<std::uint8_t> batch_vertices, expansion;
    void textures_changed() { last_textures.valid = false; batch.valid = false; }
    GLuint vao = 0, vbo = 0, ibo = 0, ubo = 0;
    GLint uniform_alignment = 256;
    // GL-thread vertex arrays for resident geometry, by buffer and stream format.
    std::unordered_map<std::uint64_t, GLuint> geometry_vaos;

    Impl(GlesDirectLimits l, FramebufferFetch f, Target t) : limits(l), fetch(f), target(std::move(t)) {
        if (!target.format) throw std::invalid_argument("direct renderer requires an EFB format callback");
        if (limits.shading == DirectShading::fast && !limits.program_cache.empty())
            program_list = std::make_unique<ProgramList>(limits.program_cache + "/programs.list");
        run_gl([this] {
            glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &uniform_alignment);
            if (uniform_alignment <= 0) uniform_alignment = 256;
            glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo); glGenBuffers(1, &ibo); glGenBuffers(1, &ubo);
            glBindVertexArray(vao); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo); glBindVertexArray(0);
            program_cache = std::make_unique<GlesProgramCache>(limits.program_cache);
            gl_check("direct renderer initialization");
            if (limits.shading == DirectShading::fast && program_cache->enabled() && limits.background_programs)
                builder = std::make_unique<GlesProgramBuilder>(*program_cache, limits.program_lists, fetch, 2u, limits.builder_policy);
        }, true);
    }
    ~Impl() {
        for (auto& g : geometries) delete_geometry(g);
        for (auto& g : retired_geometries) delete_geometry(g);
        try {
            run_gl([this] {
                for (auto& op : pending) op();
                for (auto& op : retiring) op();
                for (const auto& p : programs) if (p->name) glDeleteProgram(p->name);
                for (const auto& t : textures) if (t->name) glDeleteTextures(1, &t->name);
                for (const auto& s : samplers) glDeleteSamplers(1, &s.second);
                for (const auto& c : copies) { glDeleteFramebuffers(1, &c.second.framebuffer); glDeleteTextures(1, &c.second.texture); }
                for (const auto& s : spare_targets) { glDeleteFramebuffers(1, &s.framebuffer); glDeleteTextures(1, &s.texture); }
                for (const auto& a : arenas) if (a.buffer) glDeleteBuffers(1, &a.buffer);
                for (const auto& v : geometry_vaos) glDeleteVertexArrays(1, &v.second);
                glDeleteTextures(GLsizei(names.textures.size()), names.textures.data());
                glDeleteBuffers(GLsizei(names.buffers.size()), names.buffers.data());
                glDeleteSamplers(GLsizei(names.samplers.size()), names.samplers.data());
                glDeleteFramebuffers(GLsizei(names.framebuffers.size()), names.framebuffers.data());
                glDeleteBuffers(1, &vbo); glDeleteBuffers(1, &ibo); glDeleteBuffers(1, &ubo); glDeleteVertexArrays(1, &vao);
            }, true);
        } catch (...) {}
    }
    // Runs GL work after all previously submitted work, on the thread owning the context.
    void run_gl(std::function<void()> op, bool wait) {
        if (target.execute) target.execute(std::move(op), wait);
        else op();
    }
    GLuint take(Kind kind) {
        auto pool = [&]() -> std::vector<GLuint>& {
            switch (kind) {
            case Kind::texture: return names.textures;
            case Kind::buffer: return names.buffers;
            case Kind::sampler: return names.samplers;
            default: return names.framebuffers;
            }
        };
        std::unique_lock<std::mutex> lock(names.mutex);
        if (pool().empty()) {
            lock.unlock();
            run_gl([this] { refill(); }, true);
            lock.lock();
        }
        const auto name = pool().back();
        pool().pop_back();
        if (pool().size() < 16 && !names.refilling) {
            names.refilling = true;
            lock.unlock();
            run_gl([this] { refill(); }, false);
        }
        return name;
    }
    // GL thread: tops every pool up to 64 names.
    void refill() {
        std::vector<GLuint>* pools[] = {&names.textures, &names.buffers, &names.samplers, &names.framebuffers};
        std::size_t need[4];
        {
            std::lock_guard<std::mutex> lock(names.mutex);
            for (unsigned i = 0; i < 4; ++i) need[i] = 64 - std::min<std::size_t>(pools[i]->size(), 64);
        }
        GLuint fresh[4][64];
        if (need[0]) glGenTextures(GLsizei(need[0]), fresh[0]);
        if (need[1]) glGenBuffers(GLsizei(need[1]), fresh[1]);
        if (need[2]) glGenSamplers(GLsizei(need[2]), fresh[2]);
        if (need[3]) glGenFramebuffers(GLsizei(need[3]), fresh[3]);
        std::lock_guard<std::mutex> lock(names.mutex);
        for (unsigned i = 0; i < 4; ++i) pools[i]->insert(pools[i]->end(), fresh[i], fresh[i] + need[i]);
        names.refilling = false;
    }
    void retire_texture(GLuint name) { if (name) retiring.push_back([name] { glDeleteTextures(1, &name); }); }
    void retire_framebuffer(GLuint name) { if (name) retiring.push_back([name] { glDeleteFramebuffers(1, &name); }); }
    // GL thread: a buffer's vertex arrays go with it.
    void delete_buffer(GLuint name) {
        for (auto it = geometry_vaos.begin(); it != geometry_vaos.end();) {
            if ((it->first >> 32) == name) { glDeleteVertexArrays(1, &it->second); it = geometry_vaos.erase(it); }
            else ++it;
        }
        glDeleteBuffers(1, &name);
    }
    void retire_buffer(GLuint name) { if (name) retiring.push_back([this, name] { delete_buffer(name); }); }
    Program& program(const State& state, const Draw& draw) {
        if (!(last_registers.valid && last_registers.bp == state.bp_revision()))
            last_registers = {true, state.bp_revision(), direct_program_register_word(state, limits.shading)};
        const auto registers = last_registers.word;
        if (last_program.valid && last_program.bp == state.bp_state_revision() && last_program.cp == state.cp_layout_revision() &&
            last_program.xf == state.xf_register_revision() && last_program.vat == draw.vat && last_program.registers == registers &&
            last_program.latch == latch_components) {
            last_program.program->used = ++sequence; last_program.program->segment = segment;
            return *last_program.program;
        }
        direct_program_words(state, draw.layout, fetch, limits.shading, words, latch_components);
        const auto hash = hash_words(words);
        Program* found = nullptr;
        const auto range = program_index.equal_range(hash);
        for (auto it = range.first; it != range.second; ++it) if (it->second->words == words) { found = it->second; break; }
        if (!found) {
            auto planned = plan_direct_program(state, draw.layout, fetch, limits.shading, false, latch_components);
            if (!planned) throw std::runtime_error(planned.status.message);
            found = &create_program(hash, std::move(planned.program));
            found->batchable = batchable(*found->plan);
            found->instanceable = instanceable(*found->plan);
        }
        found->used = ++sequence; found->segment = segment;
        last_program = {true, state.bp_state_revision(), state.cp_layout_revision(), state.xf_register_revision(), registers, draw.vat, found,
                        latch_components};
        return *found;
    }
    // Eye-space positions are exact for any pipeline that reads no object-space
    // normal or position input; TEV registers then travel per vertex.
    static bool batchable(const DirectProgram& p) {
        if (!p.fast || p.transform.normal_components) return false;
        for (unsigned i = 0; i < p.transform.texgen_count; ++i)
            if (p.transform.texgens[i].type < 2 && p.transform.texgens[i].source_row < 5) return false;
        return true;
    }
    Program* batched_program(Program& base, const State& state, const Draw& draw) {
        if (!base.batched) {
            words = base.words;
            words.push_back(batched_program_word);
            const auto hash = hash_words(words);
            Program* found = nullptr;
            const auto range = program_index.equal_range(hash);
            for (auto it = range.first; it != range.second; ++it) if (it->second->words == words) { found = it->second; break; }
            if (!found) {
                auto planned = plan_direct_program(state, draw.layout, fetch, limits.shading, true);
                if (!planned) { base.batchable = false; return nullptr; }
                found = &create_program(hash, std::move(planned.program));
            }
            base.batched = found;
        }
        base.batched->used = ++sequence; base.batched->segment = segment;
        return base.batched;
    }
    // Fast programs without emboss texgens, which read the normal matrix outside the
    // instanced rows; see generate_transform_glsl.
    static bool instanceable(const DirectProgram& p) {
        if (!p.fast || p.batched) return false;
        for (unsigned i = 0; i < p.transform.texgen_count; ++i)
            if (p.transform.texgens[i].type == 1) return false;
        return true;
    }
    Program* instanced_program(Program& base, const State& state, const Draw& draw) {
        if (!base.instanced) {
            words = base.words;
            words.push_back(instanced_program_word);
            const auto hash = hash_words(words);
            Program* found = nullptr;
            const auto range = program_index.equal_range(hash);
            for (auto it = range.first; it != range.second; ++it) if (it->second->words == words) { found = it->second; break; }
            if (!found) {
                auto planned = plan_direct_program(state, draw.layout, fetch, limits.shading, false, latch_components, true);
                if (!planned) { base.instanceable = false; return nullptr; }
                found = &create_program(hash, std::move(planned.program));
            }
            base.instanced = found;
        }
        base.instanced->used = ++sequence; base.instanced->segment = segment;
        return base.instanced;
    }
    // The block bytes outside the instanced rows: position rows pn..pn+2 and normal rows
    // pn % 32 .. +2 (which the shader reads at those indices even past the normal array).
    static bool same_outside_instance_rows(const std::uint8_t* a, const std::uint8_t* b, unsigned pn) {
        const std::size_t position = std::size_t{pn} * 16, normal = offsetof(TransformUniforms, normal) + std::size_t{pn % 32} * 16;
        const std::size_t first = std::min(position, normal), second = std::max(position, normal);
        if (std::memcmp(a, b, first)) return false;
        if (std::memcmp(a + first + 48, b + first + 48, second - first - 48)) return false;
        return !std::memcmp(a + second + 48, b + second + 48, sizeof(TransformUniforms) - second - 48);
    }
    void append_instance_rows(const std::uint8_t* block, unsigned pn) {
        const auto at = uniforms.size();
        uniforms.resize(at + transform_instance_rows * 16);
        std::memcpy(uniforms.data() + at, block + std::size_t{pn} * 16, 48);
        std::memcpy(uniforms.data() + at + 48, block + offsetof(TransformUniforms, normal) + std::size_t{pn % 32} * 16, 48);
    }
    // Adds the draw as an instance of the previous record when both draw the same resident
    // geometry with the same program, state, textures and latches, and their transform
    // blocks differ only in the draw's position and normal matrix. The previous record
    // becomes an instanced draw of the program's instanced variant on its first join.
    struct { bool open = false; std::uint32_t record = 0, base = 0; unsigned pn = 0; } instance_group;
    bool join_instances(Program& p, const State& state, const Draw& draw, const GeometryDraw& geometry, std::uint32_t count,
                        std::uint32_t low, std::uint32_t high, const std::array<std::uint32_t, 3>& latches) {
        if (!p.instanceable || records.empty() || geometry.format.matrices) return false;
        const unsigned pn = state.matrix_index(0);
        if (pn > 61) return false;
        const auto& transform = p.plan->transform;
        for (unsigned i = 0; i < transform.texgen_count; ++i) {
            if (transform.texgens[i].type >= 2) continue;
            const unsigned row = state.matrix_index(static_cast<std::uint8_t>(1 + i));
            if (row + 3 > pn && row < pn + 3) return false;
        }
        auto& last = records.back();
        const auto index = static_cast<std::uint32_t>(records.size() - 1);
        const bool grouped = instance_group.open && instance_group.record == index;
        // Games rewrite unchanged state between draws: a new plan with the same content is the same state.
        const bool same_state = last.state == last_state.index ||
            !std::memcmp(&states[last.state], &states[last_state.index], sizeof(DirectDrawState));
        if (!last.resident || !(last.program == &p || (grouped && last.program == p.instanced)) || !same_state ||
            last.textures != last_textures.textures || last.samplers != last_textures.samplers || last.latches != latches ||
            last.geometry.buffer != geometry.buffer || last.geometry.base_vertex != geometry.base_vertex ||
            last.geometry.index_offset != geometry.index_offset || last.geometry.short_indices != geometry.short_indices ||
            last.geometry.format != geometry.format || last.count != count || last.low != low || last.high != high)
            return false;
        if (grouped && (instance_group.pn != pn || last.instances >= transform_instance_limit ||
                        uniforms.size() != std::size_t{last.instance_offset} + std::size_t{last.instances} * transform_instance_rows * 16))
            return false;
        if (!grouped && (last.instances || last.uniform + sizeof(TransformUniforms) > uniforms.size())) return false;
        const auto* base = uniforms.data() + (grouped ? instance_group.base : last.uniform);
        const auto* block = reinterpret_cast<const std::uint8_t*>(&transform_block);
        if (!same_outside_instance_rows(base, block, pn)) return false;
        if (!grouped) {
            Program* variant = instanced_program(p, state, draw);
            if (!variant) return false;
            // The first instance's rows come from its own block.
            const auto first = (uniforms.size() + uniform_alignment - 1) / uniform_alignment * uniform_alignment;
            uniforms.resize(first);
            instance_group = {true, index, last.uniform, pn};
            last.program = variant;
            last.instance_offset = static_cast<std::uint32_t>(first);
            last.instances = 1;
            append_instance_rows(uniforms.data() + last.uniform, pn);
        }
        append_instance_rows(block, pn);
        ++last.instances;
        ++stats.draws; ++stats.instanced_draws;
        if (last_state.index != last.state && last_state.index + 1 == states.size()) {
            states.pop_back();
            last_state.index = last.state;
        }
        return true;
    }
    // Registers a program for words; it is linked on the GL thread before its first draw.
    Program& create_program(std::uint64_t hash, std::shared_ptr<const DirectProgram> plan) {
        auto entry = std::make_unique<Program>();
        entry->words = words; entry->hash = hash; entry->plan = std::move(plan);
        Program* found = entry.get();
        programs.push_back(std::move(entry));
        program_index.emplace(hash, found);
        ++stats.program_misses;
        found->used = ++sequence; found->segment = segment;
        evict_programs(found);
        return *found;
    }
    // GL thread.
    void link_program(Program& p) {
        const auto start = std::chrono::steady_clock::now();
        p.name = gx::link_program(p.plan->vertex, p.plan->fragment, *program_cache);
        // Programs are listed when first linked: for a drawn record or a warm-up (see warm).
        if (program_list) program_list->record(p.words);
        link_us += std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        const auto block = glGetUniformBlockIndex(p.name, "GXTransform");
        if (block == GL_INVALID_INDEX) throw std::runtime_error("missing GX transform uniform block");
        glUniformBlockBinding(p.name, block, 0); glUseProgram(p.name);
        auto location = [&](const std::string& name) { return glGetUniformLocation(p.name, name.c_str()); };
        for (unsigned i = 0; i < 4; ++i) {
            p.registers[i].location = location("uGxTevRegisters[" + std::to_string(i) + "]");
            p.konst[i].location = location("uGxTevKonst[" + std::to_string(i) + "]");
            p.indirect_scales[i].location = location("uGxIndirectScale[" + std::to_string(i) + "]");
        }
        for (unsigned i = 0; i < 16; ++i) {
            p.scales[i].location = location("uGxCoordinateScale[" + std::to_string(i) + "]");
            p.image_sizes[i].location = location("uGxImageSize[" + std::to_string(i) + "]");
        }
        for (unsigned i = 0; i < 6; ++i) p.indirect_rows[i].location = location("uGxIndirectRows[" + std::to_string(i) + "]");
        for (unsigned i = 0; i < 8; ++i) p.sampler_lod[i].location = location("uGxSamplerLod[" + std::to_string(i) + "]");
        for (unsigned i = 0; i < 8; ++i) p.bias[i].location = location("uGxSamplerBias[" + std::to_string(i) + "]");
        p.constant_alpha.location = location("uGxConstantAlpha");
        p.fog.location = location("uGxFog");
        p.fog_color.location = location("uGxFogColor");
        p.fog_center.location = location("uGxFogCenter");
        p.fog_table.location = location("uGxFogTable[0]");
        p.alpha.location = location("uGxTevAlphaRef");
        p.pixel.location = location("uGxPixel");
        p.z_bias.location = location("uGxZBias");
        for (unsigned i = 0; i < 8; ++i) glUniform1i(location("uGxTexture" + std::to_string(i)), i);
        gl_check("direct GX program");
    }
    // GL thread: links p once its binary can be loaded. Without one, the builder compiles it
    // while draws using p are skipped, instead of stalling frames on the compile.
    bool ready(Program& p) {
        if (p.unbuilt == gl_segments || p.rejected) return false;
        const auto& v = p.plan->vertex; const auto& f = p.plan->fragment;
        const auto now = std::chrono::steady_clock::now();
        bool fallback = false;
        if (builder && builder->running() && !program_cache->contains(v, f) && !(p.requested && !builder->building(v, f))) {
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.requested_at);
            const auto since_link = last_fallback == std::chrono::steady_clock::time_point{} ? std::chrono::milliseconds::max()
                                  : std::chrono::duration_cast<std::chrono::milliseconds>(now - last_fallback);
            if (!link_unready_program(p.requested, waited, limits.unready_link_ms, since_link, limits.unready_link_interval_ms)) {
                if (!p.requested) { p.requested = true; p.requested_at = now; builder->request(v, f); }
                p.unbuilt = gl_segments;
                return false;
            }
            fallback = true;
        }
        try {
            link_program(p);
        } catch (const std::exception& e) {
            // A driver that rejects one generated program costs that program's draws, not the game.
            p.rejected = true;
            if (++rejected_programs <= 8) std::fprintf(stderr, "Shader rejected by the driver, its draws are skipped: %s\n", e.what());
            return false;
        }
        if (p.requested) note_wait(p, now, fallback);
        if (fallback) last_fallback = std::chrono::steady_clock::now();
        return true;
    }
    // A program its draws waited for is ready: how long they waited, and the GL thread's link
    // when it linked the program itself.
    void note_wait(const Program& p, std::chrono::steady_clock::time_point link_start, bool fallback) {
        using std::chrono::duration_cast; using std::chrono::milliseconds;
        const auto end = std::chrono::steady_clock::now();
        const auto waited = std::uint64_t(duration_cast<milliseconds>(end - p.requested_at).count());
        const std::uint64_t bounds[] = {33, 67, 133, 267};
        unsigned bucket = 0;
        while (bucket < 4 && waited > bounds[bucket]) ++bucket;
        ++program_waits[bucket];
        auto raise = [](std::atomic<std::uint64_t>& max, std::uint64_t value) {
            for (auto old = max.load(); value > old && !max.compare_exchange_weak(old, value);) {}
        };
        raise(program_wait_max_ms, waited);
        if (!fallback) return;
        const auto link = std::uint64_t(duration_cast<milliseconds>(end - link_start).count());
        raise(fallback_link_max_ms, link);
        if (++fallback_links <= 16)
            std::fprintf(stderr, "Shader linked on the GL thread after its draws waited %llu ms (link %llu ms)\n",
                         (unsigned long long)(waited - link), (unsigned long long)link);
    }
    // Draws culled before the GPU still start their program's build, so geometry that moves into
    // view, such as a model entering at a screen edge, does not stay missing while it builds.
    void warm(Program& p) {
        if (p.warmed == segment) return;
        p.warmed = segment;
        warming.push_back(&p);
    }
    void evict_programs(const Program* keep) {
        while (programs.size() > limits.programs) {
            auto oldest = programs.end();
            for (auto it = programs.begin(); it != programs.end(); ++it)
                if (it->get() != keep && (*it)->segment != segment && (oldest == programs.end() || (*it)->used < (*oldest)->used)) oldest = it;
            if (oldest == programs.end()) return;
            const auto range = program_index.equal_range((*oldest)->hash);
            for (auto it = range.first; it != range.second; ++it) if (it->second == oldest->get()) { program_index.erase(it); break; }
            for (auto& other : programs) {
                if (other->batched == oldest->get()) other->batched = nullptr;
                if (other->instanced == oldest->get()) other->instanced = nullptr;
            }
            if (batch.program == oldest->get()) batch.valid = false;
            // Queued segments may still draw with it; the GL thread deletes it after them.
            std::shared_ptr<Program> retired(oldest->release());
            retiring.push_back([retired] { if (retired->name) glDeleteProgram(retired->name); });
            programs.erase(oldest);
            last_program.valid = false;
        }
    }
    GLuint sampler(const TextureSampler& s) {
        const bool fast = limits.shading == DirectShading::fast;
        std::uint32_t key = std::uint32_t(s.wrap_s) | (std::uint32_t(s.wrap_t) << 2) |
                            (std::uint32_t(s.min_filter) << 4) | (std::uint32_t(s.linear_magnification) << 7);
        if (fast) key |= (std::uint32_t(s.min_lod_sixteenths) << 8) | (std::uint32_t(s.max_lod_sixteenths) << 20);
        for (const auto& entry : samplers) if (entry.first == key) return entry.second;
        const GLuint name = take(Kind::sampler);
        pending.push_back([name, s, fast] {
            glSamplerParameteri(name, GL_TEXTURE_MIN_FILTER, min_filter(s.min_filter));
            glSamplerParameteri(name, GL_TEXTURE_MAG_FILTER, s.linear_magnification ? GL_LINEAR : GL_NEAREST);
            glSamplerParameteri(name, GL_TEXTURE_WRAP_S, wrap(s.wrap_s));
            glSamplerParameteri(name, GL_TEXTURE_WRAP_T, wrap(s.wrap_t));
            if (fast) {
                glSamplerParameterf(name, GL_TEXTURE_MIN_LOD, s.min_lod_sixteenths / 16.0f);
                glSamplerParameterf(name, GL_TEXTURE_MAX_LOD, s.max_lod_sixteenths / 16.0f);
            }
        });
        samplers.emplace_back(key, name);
        return name;
    }
    void upload(Texture& entry, std::shared_ptr<const TextureSnapshot> image) {
        const auto& levels = image->levels();
        retire_texture(entry.name);
        stats.texture_bytes -= entry.bytes;
        entry.bytes = 0;
        for (const auto& level : levels) entry.bytes += level.rgba.size();
        entry.name = take(Kind::texture);
        pending.push_back([name = entry.name, image] {
            const auto& levels = image->levels();
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, name);
            glTexStorage2D(GL_TEXTURE_2D, static_cast<GLsizei>(levels.size()), GL_RGBA8, levels[0].width, levels[0].height);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glPixelStorei(GL_UNPACK_SKIP_ROWS, 0); glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
            for (unsigned lod = 0; lod < levels.size(); ++lod) {
                const auto& level = levels[lod];
                glTexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(lod), 0, 0, level.width, level.height, GL_RGBA, GL_UNSIGNED_BYTE, level.rgba.data());
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(levels.size() - 1));
        });
        stats.texture_bytes += entry.bytes;
        ++stats.texture_uploads;
        const auto& last = levels.back();
        entry.source_bytes = last.source_offset + last.source_size;
    }
    GLuint texture(const State& state, unsigned map, const TextureDescriptor& d, MemoryReader& memory, TextureCmprModel cmpr) {
        ++stats.texture_lookups;
        TextureKey key;
        key.address = d.address; key.image = d.bp_words[2];
        const auto filter = static_cast<unsigned>(d.sampler.min_filter);
        if (filter != 0 && filter != 4) {
            unsigned width = d.width, height = d.height, count = 0;
            const unsigned last = (d.sampler.max_lod_sixteenths + 15u) / 16u;
            for (unsigned lod = 0; lod <= last; ++lod) { ++count; if (width == 1 && height == 1) break; width = std::max(1u, width / 2); height = std::max(1u, height / 2); }
            key.levels = count;
        } else key.levels = 1;
        if (const auto bytes = palette_bytes(d.format)) {
            key.palette = d.bp_words[6];
            // Unloaded 32-byte chunks hash as a marker; decoding rejects any that are referenced.
            scratch.assign(bytes + bytes / 32, 0);
            for (std::size_t chunk = 0; chunk < bytes / 32; ++chunk)
                scratch[bytes + chunk] = state.read_palette(d.palette_offset + chunk * 32, scratch.data() + chunk * 32, 32);
            key.palette_hash = hash_bytes(scratch.data(), scratch.size());
        }
        if (d.format == TextureFormat::cmpr) key.model = cmpr;
        if (!copies.empty()) if (const auto name = resident(d, key.levels)) return name;
        const std::uint64_t fields[] = {key.address, key.image, key.levels, key.palette, key.palette_hash,
                                        static_cast<std::uint64_t>(key.model)};
        const auto key_hash = hash_bytes(reinterpret_cast<const std::uint8_t*>(fields), sizeof fields);
        Texture* entry = nullptr;
        const auto range = texture_index.equal_range(key_hash);
        for (auto it = range.first; it != range.second; ++it) if (it->second->key == key) { entry = it->second; break; }
        auto content = [&](Texture& t) {
            ByteView view = memory.view(d.address, t.source_bytes);
            if (!view.data || view.size != t.source_bytes) {
                scratch.resize(t.source_bytes);
                if (!memory.copy(d.address, scratch.data(), t.source_bytes)) throw std::runtime_error("GX texture source read failed");
                view = {scratch.data(), scratch.size()};
                t.pointer = nullptr;
            } else t.pointer = view.data;
            stats.texture_hash_bytes += view.size;
            return hash_bytes(view.data, view.size);
        };
        auto decode = [&](Texture& t) {
            const auto captured = capture_texture(state, static_cast<std::uint8_t>(map), memory, cmpr, {}, nullptr);
            if (!captured) throw std::runtime_error(captured.message);
            upload(t, captured.snapshot);
            t.content = content(t);
        };
        if (entry) {
            if (limits.verify_textures || entry->validated != frame) {
                const auto generation = entry->pointer ? memory.source_generation(entry->pointer) : 0;
                const bool full = limits.verify_textures || !generation || entry->volatile_data || generation != entry->generation ||
                                  ((frame + (entry->key_hash & 15)) & 15) == 0;
                if (full) {
                    ++stats.texture_validations;
                    if (content(*entry) != entry->content) {
                        ++stats.texture_mutations;
                        if (generation && generation == entry->generation) entry->volatile_data = true;
                        decode(*entry);
                    }
                    entry->generation = entry->pointer ? memory.source_generation(entry->pointer) : 0;
                }
                entry->validated = frame;
            }
        } else {
            auto created = std::make_unique<Texture>();
            created->key = key; created->key_hash = key_hash;
            decode(*created);
            created->generation = created->pointer ? memory.source_generation(created->pointer) : 0;
            created->validated = frame;
            entry = created.get();
            textures.push_back(std::move(created));
            texture_index.emplace(key_hash, entry);
        }
        entry->used = ++sequence; entry->segment = segment;
        evict_textures();
        return entry->name;
    }
    // Overlapping resident copies within [begin, end), in token order.
    std::vector<std::map<std::uint32_t, GlesResidentCopy>::iterator> overlapping(std::uint32_t begin, std::size_t bytes) {
        std::vector<std::map<std::uint32_t, GlesResidentCopy>::iterator> found;
        auto it = copies.upper_bound(begin);
        if (it != copies.begin()) --it;
        for (; it != copies.end() && it->first < std::uint64_t{begin} + bytes; ++it)
            if (std::uint64_t{it->first} + it->second.bytes > begin) found.push_back(it);
        return found;
    }
    void materialize(std::map<std::uint32_t, GlesResidentCopy>::iterator it) {
        target.materialize(it->second);
        ++stats.materialized;
        retire_texture(it->second.texture);
        retire_framebuffer(it->second.framebuffer);
        invalidate_pointer(it->second.destination, it->second.bytes);
        copies.erase(it);
    }
    GLuint resident(const TextureDescriptor& d, unsigned levels) {
        const auto bytes = texture_source_bytes(d, levels);
        auto found = overlapping(d.address, bytes);
        if (found.empty()) return 0;
        if (found.size() == 1) {
            const auto& c = found[0]->second;
            const auto& p = c.plan;
            const unsigned tile_width = d.format == TextureFormat::i8 ? 8 : 4, tile_bytes = d.format == TextureFormat::rgba8 ? 64 : 32;
            if (found[0]->first == d.address && levels == 1 && static_cast<unsigned>(d.format) == texture_copy_sample_format(p) &&
                d.width == p.destination_width() && d.height == p.destination_height() &&
                p.stride == std::size_t{(d.width + tile_width - 1u) / tile_width} * tile_bytes && c.intact()) {
                ++stats.resident_hits;
                return c.texture;
            }
        }
        for (auto it : found) materialize(it);
        textures_changed();
        return 0;
    }
    void invalidate_pointer(const std::uint8_t* begin, std::size_t size) {
        for (auto& t : textures)
            if (!t->pointer || (t->pointer < begin + size && begin < t->pointer + t->source_bytes)) { t->validated = 0; t->generation = 0; }
        textures_changed();
    }
    void retire_target(const GlesResidentCopy& c) {
        spare_targets.push_back({int(c.plan.destination_width()), int(c.plan.destination_height()), c.texture, c.framebuffer, frame});
        // Bounded: the oldest spares beyond a few per frame's copies are deleted.
        if (spare_targets.size() > 48) {
            retire_texture(spare_targets.front().texture); retire_framebuffer(spare_targets.front().framebuffer);
            spare_targets.erase(spare_targets.begin());
        }
    }
    GlesDirect::ResidentTarget resident_copy(const TextureCopy& p, std::uint32_t token, std::uint8_t* destination, std::size_t capacity) {
        const auto bytes = texture_copy_bytes(p);
        for (auto it : overlapping(token, bytes)) {
            const auto& c = it->second;
            if (it->first >= token && std::uint64_t{it->first} + c.bytes <= std::uint64_t{token} + bytes) {
                retire_target(c); copies.erase(it);
            } else materialize(it);
        }
        const int width = p.destination_width(), height = p.destination_height();
        GLuint texture = 0, framebuffer = 0;
        for (auto it = spare_targets.begin(); it != spare_targets.end(); ++it)
            if (it->width == width && it->height == height && it->frame + 2 <= frame) {
                texture = it->texture; framebuffer = it->framebuffer; spare_targets.erase(it); break;
            }
        if (!texture) {
            texture = take(Kind::texture); framebuffer = take(Kind::framebuffer);
            pending.push_back([texture, framebuffer, width, height] {
                glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, texture);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                    throw std::runtime_error("resident texture-copy framebuffer is incomplete");
            });
        }
        auto& entry = copies[token];
        entry = {token, bytes, texture, framebuffer, p, destination, capacity};
        if (limits.stamp_resident_copies) { entry.stamp = ++stamps; entry.write_stamps(); }
        invalidate_pointer(destination, bytes);
        textures_changed();
        ++stats.resident_copies;
        return {texture, framebuffer};
    }
    void release(std::uint32_t token, std::size_t bytes) {
        for (auto it : overlapping(token, bytes)) { retire_target(it->second); copies.erase(it); }
        textures_changed();
    }
    void evict_textures() {
        while (textures.size() > limits.textures || stats.texture_bytes > limits.texture_bytes) {
            auto oldest = textures.end();
            for (auto it = textures.begin(); it != textures.end(); ++it)
                if ((*it)->segment != segment && (oldest == textures.end() || (*it)->used < (*oldest)->used)) oldest = it;
            if (oldest == textures.end()) return; // Everything is pending; evict after submission.
            const auto range = texture_index.equal_range((*oldest)->key_hash);
            for (auto it = range.first; it != range.second; ++it) if (it->second == oldest->get()) { texture_index.erase(it); break; }
            retire_texture((*oldest)->name);
            stats.texture_bytes -= (*oldest)->bytes;
            textures.erase(oldest);
            textures_changed();
        }
    }
    bool cacheable(const Draw& draw) const {
        if (draw.total_vertices() < std::max(3u, limits.geometry_min_vertices)) return false;
        const auto& l = draw.layout;
        auto indexed = [](const Attribute& a) { return a.mode == AttributeMode::index8 || a.mode == AttributeMode::index16; };
        if (indexed(l.position) || indexed(l.normal)) return true;
        for (const auto& a : l.colors) if (indexed(a)) return true;
        for (const auto& a : l.texcoords) if (indexed(a)) return true;
        return false;
    }
    std::uint64_t geometry_hash(const Draw& draw) const {
        return hash_bytes(draw.vertices.data, draw.vertices.size) ^ (hash_words(geometry_key) * 0x9e3779b97f4a7c15ull);
    }
    void build_geometry_key(const State& state, const Draw& draw, const StreamFormat& format) {
        auto& k = geometry_key; k.clear();
        k.push_back(draw.primitive | (std::uint32_t{draw.vat} << 8) | (std::uint32_t{draw.vertex_count} << 16));
        // A merged draw's payload holds each later run's header, so its bytes already
        // name the runs; the count separates it from a single primitive.
        k.push_back(draw.run_count);
        for (unsigned a : {0x50u, 0x60u, 0x70u + draw.vat, 0x80u + draw.vat, 0x90u + draw.vat}) k.push_back(state.cp(static_cast<std::uint8_t>(a)));
        for (unsigned i = 0; i < 12; ++i) {
            k.push_back(state.cp(static_cast<std::uint8_t>(0xa0 + i)));
            k.push_back(state.cp(static_cast<std::uint8_t>(0xb0 + i)) & 255);
        }
        if (format.matrices) for (auto w : stream_matrix_latches(state)) k.push_back(w);
    }
    // Unchanged reported-write generations prove the span unchanged, except on the
    // entry's periodic full comparison, which also detects unreported writers.
    bool span_valid(Geometry::Span& span, MemoryReader& memory, bool full) {
        const auto view = memory.array_view(span.base);
        if (!view.bytes.data || !same_encoding(view, span.native_unit, span.host_order) || span.end > view.bytes.size)
            return false;
        const auto generation = memory.range_generation(view, span.base + span.first, span.end - span.first);
        if (!full && generation && generation == span.generation) return true;
        stats.geometry_validated_bytes += span.end - span.first;
        if (std::memcmp(view.bytes.data + span.first, span.bytes.data(), span.bytes.size())) {
            if (generation == span.generation) volatile_bases.insert(span.base);
            return false;
        }
        span.generation = generation;
        return true;
    }
    // Same display-list payload as a cached entry without hashing or comparing it.
    Geometry* find_geometry_by_pointer(const Draw& draw, MemoryReader& memory) {
        source = memory.source_identity(draw.vertices.data, draw.vertices.size);
        if (!source.generation) return nullptr;
        const auto range = geometry_pointers.equal_range(source.key);
        if (range.first == range.second) return nullptr;
        const auto generation = source.generation;
        for (auto it = range.first; it != range.second; ++it) {
            const auto entry = it->second;
            if (entry->fifo_generation != generation || entry->fifo.size() != draw.vertices.size || entry->key != geometry_key) continue;
            // The periodic full comparison also covers list bytes rewritten without a reported write.
            if (((frame + entry->phase) & full_check_period) == 0 && std::memcmp(entry->fifo.data(), draw.vertices.data, draw.vertices.size)) continue;
            const auto index = geometry_index.equal_range(entry->hash);
            for (auto at = index.first; at != index.second; ++at) if (at->second == entry) return validate_geometry(at, memory, entry->hash);
        }
        return nullptr;
    }
    Geometry* validate_geometry(std::unordered_multimap<std::uint64_t, std::list<Geometry>::iterator>::iterator it,
                                MemoryReader& memory, std::uint64_t hash) {
        const auto entry = it->second;
        bool valid = true;
        const bool full = entry->volatile_data || ((frame + entry->phase) & full_check_period) == 0;
        for (auto& span : entry->spans) if (!span_valid(span, memory, full)) { valid = false; break; }
        if (!valid) {
            if (unstable_geometry.size() > 4096) unstable_geometry.clear();
            auto& changes = unstable_geometry[hash];
            if (changes < 255) ++changes;
            erase_geometry(it);
            return nullptr;
        }
        geometries.splice(geometries.begin(), geometries, entry);
        entry->segment = segment;
        ++stats.geometry_hits;
        return &*entry;
    }
    Geometry* find_geometry(const Draw& draw, MemoryReader& memory, std::uint64_t hash) {
        const auto range = geometry_index.equal_range(hash);
        for (auto it = range.first; it != range.second; ++it) {
            const auto entry = it->second;
            if (entry->key != geometry_key || entry->fifo.size() != draw.vertices.size ||
                std::memcmp(entry->fifo.data(), draw.vertices.data, draw.vertices.size)) continue;
            const auto found = validate_geometry(it, memory, hash);
            // The compared bytes now stand for the payload's source at its current generation.
            if (found && source.generation) name_geometry(entry);
            return found;
        }
        return nullptr;
    }
    void name_geometry(std::list<Geometry>::iterator g) {
        if (g->fifo_pointer == source.key) { g->fifo_generation = source.generation; return; }
        if (g->fifo_pointer) {
            const auto range = geometry_pointers.equal_range(g->fifo_pointer);
            for (auto at = range.first; at != range.second; ++at) if (at->second == g) { geometry_pointers.erase(at); break; }
        }
        g->fifo_pointer = source.key;
        g->fifo_generation = source.generation;
        geometry_pointers.emplace(source.key, g);
    }
    // Pending records may still name the entry, so its buffers retire after the segment.
    void erase_geometry(std::unordered_multimap<std::uint64_t, std::list<Geometry>::iterator>::iterator it) {
        if (it->second->fifo_pointer) {
            const auto range = geometry_pointers.equal_range(it->second->fifo_pointer);
            for (auto at = range.first; at != range.second; ++at) if (at->second == it->second) { geometry_pointers.erase(at); break; }
        }
        stats.geometry_bytes -= it->second->bytes;
        retired_geometries.splice(retired_geometries.end(), geometries, it->second);
        geometry_index.erase(it);
    }
    void delete_geometry(Geometry& g) {
        if (g.arena < 0) {
            if (g.buffer) {
                retire_buffer(g.buffer);
                const auto found = std::find(loose.begin(), loose.end(), &g);
                if (found != loose.end()) { *found = loose.back(); loose.pop_back(); loose_bytes -= g.payload; }
            }
        } else {
            auto& a = arenas[static_cast<std::size_t>(g.arena)];
            a.live -= g.payload;
            if (!a.live) { retire_buffer(a.buffer); a = {}; }
        }
        g.buffer = 0; g.arena = -1;
    }
    // Packs loose entries inserted before this frame into one new arena. The arena is
    // written once before any draw uses it, so no submitted buffer is modified.
    static std::uint32_t format_key(const StreamFormat& f) {
        return std::uint32_t{f.normal_vectors} | (std::uint32_t{f.colors} << 2) | (std::uint32_t{f.texcoords} << 4) |
               (std::uint32_t{f.matrices} << 12) | (std::uint32_t{f.registers} << 13);
    }
    // GL thread: the vertex array drawing geometry from buffer in format, shared
    // by every entry packed into that buffer.
    GLuint geometry_vao(GLuint buffer, const StreamFormat& format) {
        auto& name = geometry_vaos[(std::uint64_t{buffer} << 32) | format_key(format)];
        if (name) return name;
        glGenVertexArrays(1, &name);
        glBindVertexArray(name);
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        bind_format(format, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
        glBindVertexArray(0);
        return name;
    }
    // Packs loose entries inserted before this frame into one new arena. The arena is
    // written once before any draw uses it, so no submitted buffer is modified.
    void pack_loose() {
        std::vector<Geometry*> settled;
        for (auto* g : loose) if (g->inserted < frame) settled.push_back(g);
        if (settled.size() < 64 && loose_bytes < (1u << 20)) return;
        // Vertices start on a stride multiple so base_vertex addresses them; indices follow.
        struct Place { std::size_t vertices, indices; };
        std::vector<Place> places;
        std::size_t total = 0;
        for (auto* g : settled) {
            const std::size_t stride = g->format.stride, vertex_bytes = std::size_t{g->vertex_count} * stride;
            const auto vertices = (total + stride - 1) / stride * stride;
            const auto indices = (vertices + vertex_bytes + 3) & ~std::size_t{3};
            places.push_back({vertices, indices});
            total = indices + (g->data.size() - g->index_offset);
        }
        auto packed = std::make_shared<std::vector<std::uint8_t>>(total, 0);
        std::size_t slot = 0;
        while (slot < arenas.size() && arenas[slot].buffer) ++slot;
        if (slot == arenas.size()) arenas.emplace_back();
        auto& a = arenas[slot];
        a.buffer = take(Kind::buffer);
        a.capacity = total; a.live = 0;
        for (std::size_t i = 0; i < settled.size(); ++i) {
            const auto* g = settled[i];
            std::memcpy(packed->data() + places[i].vertices, g->data.data(), std::size_t{g->vertex_count} * g->format.stride);
            std::memcpy(packed->data() + places[i].indices, g->data.data() + g->index_offset, g->data.size() - g->index_offset);
        }
        pending.push_back([buffer = a.buffer, packed] {
            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, buffer);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(packed->size()), packed->data(), GL_STATIC_DRAW);
        });
        for (std::size_t i = 0; i < settled.size(); ++i) {
            auto* g = settled[i];
            retire_buffer(g->buffer);
            g->buffer = a.buffer; g->arena = static_cast<std::int32_t>(slot);
            g->base_vertex = static_cast<std::int32_t>(places[i].vertices / g->format.stride);
            g->index_offset = static_cast<std::uint32_t>(places[i].indices);
            a.live += g->payload; loose_bytes -= g->payload;
            stats.geometry_bytes -= g->data.size(); g->bytes -= g->data.size();
            std::vector<std::uint8_t>().swap(g->data);
        }
        loose.erase(std::remove_if(loose.begin(), loose.end(), [](const Geometry* g) { return g->arena >= 0; }), loose.end());
        release_sparse_arena();
    }
    // Arenas only shrink when empty; once dead space dominates, the sparsest arena's
    // entries are dropped and reconverted on their next use.
    void release_sparse_arena() {
        std::size_t capacity = 0, live = 0, sparsest = arenas.size();
        for (std::size_t i = 0; i < arenas.size(); ++i) if (arenas[i].buffer) {
            capacity += arenas[i].capacity; live += arenas[i].live;
            if (sparsest == arenas.size() || arenas[i].live * arenas[sparsest].capacity < arenas[sparsest].live * arenas[i].capacity) sparsest = i;
        }
        if (sparsest == arenas.size() || capacity < (8u << 20) || capacity < 2 * live) return;
        for (auto it = geometries.begin(); it != geometries.end();) {
            const auto next = std::next(it);
            if (it->arena == static_cast<std::int32_t>(sparsest) && it->segment != segment) {
                const auto range = geometry_index.equal_range(it->hash);
                for (auto entry = range.first; entry != range.second; ++entry) if (entry->second == it) { erase_geometry(entry); break; }
            }
            it = next;
        }
    }
    void bind_format(const StreamFormat& f, std::uintptr_t base) {
        const auto stride = static_cast<GLsizei>(f.stride);
        auto pointer = [&](std::size_t offset) { return reinterpret_cast<const void*>(base + offset); };
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, pointer(0));
        for (unsigned i = 0; i < 3; ++i) {
            if (f.registers) {
                glEnableVertexAttribArray(1 + i);
                glVertexAttribPointer(1 + i, 4, GL_SHORT, GL_FALSE, stride, pointer(f.register_offset() + i * 8u));
            } else if (i < f.normal_vectors) {
                glEnableVertexAttribArray(1 + i);
                glVertexAttribPointer(1 + i, 3, GL_FLOAT, GL_FALSE, stride, pointer(f.normal_offset() + i * 12u));
            } else glDisableVertexAttribArray(1 + i);
        }
        if (f.registers) {
            glEnableVertexAttribArray(15);
            glVertexAttribPointer(15, 4, GL_SHORT, GL_FALSE, stride, pointer(f.register_offset() + 24u));
        } else glDisableVertexAttribArray(15);
        for (unsigned c = 0; c < 2; ++c) {
            if (f.colors & (1u << c)) {
                glEnableVertexAttribArray(4 + c);
                glVertexAttribPointer(4 + c, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, pointer(f.color_offset(c)));
            } else glDisableVertexAttribArray(4 + c);
        }
        for (unsigned t = 0; t < 8; ++t) {
            if (f.texcoords & (1u << t)) {
                glEnableVertexAttribArray(6 + t);
                glVertexAttribPointer(6 + t, 2, GL_FLOAT, GL_FALSE, stride, pointer(f.texcoord_offset(t)));
            } else glDisableVertexAttribArray(6 + t);
        }
        if (f.matrices) {
            glEnableVertexAttribArray(14);
            glVertexAttribIPointer(14, 3, GL_UNSIGNED_INT, stride, pointer(f.matrix_offset()));
        } else glDisableVertexAttribArray(14);
    }
    // Uploads the converted draw into resident buffers; null when it cannot be cached.
    Geometry* insert_geometry(const State& state, const Draw& draw, MemoryReader& memory, std::uint64_t hash,
                              const StreamFormat& format, const StreamResult& result) {
        if (!spans.viewed) return nullptr;
        Geometry g;
        g.hash = hash; g.key = geometry_key; g.format = format;
        g.fifo.assign(draw.vertices.data, draw.vertices.data + draw.vertices.size);
        g.fifo_generation = source.generation;
        if (g.fifo_generation) g.fifo_pointer = source.key;
        g.vertex_count = result.vertices; g.index_count = result.indices;
        for (unsigned a = 0; a < 12; ++a) {
            const auto& s = spans.arrays[a];
            if (s.first == s.end) continue;
            const auto base = state.cp(static_cast<std::uint8_t>(0xa0 + a));
            // Source memory is stable for the whole draw callback, so this is the content just converted.
            const auto view = memory.array_view(base);
            if (!view.bytes.data || !same_encoding(view, s.native_unit, s.host_order) || s.end > view.bytes.size) return nullptr;
            const auto generation = memory.range_generation(view, base + s.first, s.end - s.first);
            g.spans.push_back({a, base, s.first, s.end, s.native_unit, s.host_order,
                               std::vector<std::uint8_t>(view.bytes.data + s.first, view.bytes.data + s.end), generation});
            g.volatile_data |= !generation || volatile_bases.count(base) != 0;
        }
        g.phase = static_cast<std::uint8_t>(hash & full_check_period);
        merge_identical_vertices(geometry_vertices, geometry_indices, format.stride);
        g.vertex_count = static_cast<std::uint32_t>(geometry_vertices.size() / format.stride);
        if (!format.matrices) {
            g.box = bounds(geometry_vertices.data(), g.vertex_count, format.stride); g.boxed = true;
            constexpr std::uint32_t cluster_indices = 3 * 256;
            if (g.index_count >= 4 * cluster_indices) {
                for (std::uint32_t first = 0; first < g.index_count; first += cluster_indices) {
                    const auto count = std::min(cluster_indices, g.index_count - first);
                    std::array<float, 6> box{{INFINITY, INFINITY, INFINITY, -INFINITY, -INFINITY, -INFINITY}};
                    std::uint32_t low = ~0u, high = 0;
                    for (std::uint32_t i = first; i < first + count; ++i) {
                        float position[3];
                        std::memcpy(position, geometry_vertices.data() + std::size_t{geometry_indices[i]} * format.stride, sizeof position);
                        for (unsigned k = 0; k < 3; ++k) { box[k] = std::min(box[k], position[k]); box[3 + k] = std::max(box[3 + k], position[k]); }
                        low = std::min(low, geometry_indices[i]); high = std::max(high, geometry_indices[i]);
                    }
                    g.clusters.push_back({first, count, box, low, high});
                }
            }
        }
        std::size_t span_bytes = 0;
        for (const auto& span : g.spans) span_bytes += span.bytes.size();
        g.short_indices = g.vertex_count <= 65536;
        const auto index_bytes = std::size_t{g.index_count} * (g.short_indices ? 2 : 4);
        g.bytes = sizeof(Geometry) + g.fifo.size() + geometry_vertices.size() + index_bytes + g.key.size() * 4 +
                  g.spans.size() * sizeof(Geometry::Span) + span_bytes;
        if (g.bytes > limits.geometry_bytes) return nullptr;
        const auto vertex_bytes = (geometry_vertices.size() + 15) & ~std::size_t{15};
        g.data.assign(vertex_bytes + index_bytes, 0);
        std::memcpy(g.data.data(), geometry_vertices.data(), geometry_vertices.size());
        if (g.short_indices) for (std::size_t i = 0; i < geometry_indices.size(); ++i) {
            const auto narrow = static_cast<std::uint16_t>(geometry_indices[i]);
            std::memcpy(g.data.data() + vertex_bytes + i * 2, &narrow, 2);
        } else std::memcpy(g.data.data() + vertex_bytes, geometry_indices.data(), index_bytes);
        g.payload = static_cast<std::uint32_t>(g.data.size());
        g.index_offset = static_cast<std::uint32_t>(vertex_bytes);
        g.bytes += g.data.size(); // CPU copy until packed.
        g.buffer = take(Kind::buffer);
        pending.push_back([buffer = g.buffer, data = g.data] {
            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, buffer);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size()), data.data(), GL_STATIC_DRAW);
        });
        g.inserted = frame;
        g.segment = segment;
        geometries.push_front(std::move(g));
        geometry_index.emplace(hash, geometries.begin());
        stats.geometry_bytes += geometries.front().bytes;
        ++stats.geometry_misses;
        loose.push_back(&geometries.front()); loose_bytes += geometries.front().payload;
        if (geometries.front().fifo_pointer) geometry_pointers.emplace(geometries.front().fifo_pointer, geometries.begin());
        evict_geometry();
        return &geometries.front();
    }
    bool unstable(std::uint64_t hash) const {
        const auto found = unstable_geometry.find(hash);
        return found != unstable_geometry.end() && found->second >= 2;
    }
    void evict_geometry() {
        while (geometries.size() > limits.geometries || stats.geometry_bytes > limits.geometry_bytes) {
            const auto last = std::prev(geometries.end());
            if (last->segment == segment) return; // Still named by pending records.
            const auto range = geometry_index.equal_range(last->hash);
            for (auto it = range.first; it != range.second; ++it) if (it->second == last) { erase_geometry(it); break; }
        }
    }
    static std::array<float, 6> bounds(const std::uint8_t* records, std::size_t count, std::size_t stride) {
        std::array<float, 6> box{{INFINITY, INFINITY, INFINITY, -INFINITY, -INFINITY, -INFINITY}};
        for (std::size_t v = 0; v < count; ++v) {
            float position[3];
            std::memcpy(position, records + v * stride, sizeof position);
            for (unsigned k = 0; k < 3; ++k) { box[k] = std::min(box[k], position[k]); box[3 + k] = std::max(box[3 + k], position[k]); }
        }
        return box;
    }
    // True when every corner of the box, transformed by the 3x4 rows m and the XF
    // projection, lies beyond one clip plane; such geometry produces no fragments.
    // Disabled clipping and non-finite values never cull.
    static bool outside_clip(const State& state, const float* m, const std::array<float, 6>& box) {
        if (state.xf(0x1005)) return false;
        float p[6];
        for (unsigned i = 0; i < 6; ++i) { const auto word = state.xf(static_cast<std::uint16_t>(0x1020 + i)); std::memcpy(&p[i], &word, 4); }
        const bool ortho = state.xf(0x1026) != 0;
        unsigned common = 0x3f;
        for (unsigned c = 0; c < 8 && common; ++c) {
            const float x = box[c & 1 ? 3 : 0], y = box[c & 2 ? 4 : 1], z = box[c & 4 ? 5 : 2];
            const float ex = m[0] * x + m[1] * y + m[2] * z + m[3];
            const float ey = m[4] * x + m[5] * y + m[6] * z + m[7];
            const float ez = m[8] * x + m[9] * y + m[10] * z + m[11];
            const float cx = ex * p[0] + (ortho ? p[1] : ez * p[1]);
            const float cy = ey * p[2] + (ortho ? p[3] : ez * p[3]);
            const float cz = 2 * (ez * p[4] + p[5]);
            const float cw = ortho ? 1.0f : -ez;
            // A relative margin keeps geometry the GPU might still rasterize at the edge.
            const float w = cw + std::fabs(cw) * 1e-4f;
            unsigned out = 0;
            if (cx < -w) out |= 1;
            if (cx > w) out |= 2;
            if (cy < -w) out |= 4;
            if (cy > w) out |= 8;
            if (cz + cw < -w) out |= 16;
            if (cz + cw > w) out |= 32;
            common &= out;
        }
        return common != 0;
    }
    bool outside_clip(const State& state, unsigned row, const std::array<float, 6>& box) const {
        float m[12];
        std::memcpy(m, state.xf_data() + row * 4, sizeof m);
        return outside_clip(state, m, box);
    }
    // Small draws without per-vertex matrices, such as particle quads, are tested from their
    // positions alone, before any program, state or vertex conversion work. Positions are
    // dequantized as conversion does. Larger draws keep their cached boxes.
    bool early_outside_clip(const State& state, const Draw& draw, MemoryReader& memory) const {
        const auto& position = draw.layout.position;
        if (draw.merged() || position.mode == AttributeMode::none || position.format > 4 || draw.vertex_count < 3 ||
            draw.vertex_count > 64)
            return false;
        for (bool explicit_index : draw.layout.matrix_indices) if (explicit_index) return false;
        const unsigned row = state.matrix_index(0);
        if (row > 61 || draw.vertices.size < std::size_t{draw.vertex_count} * draw.layout.stride) return false;
        const unsigned size = position.format < 2 ? 1 : position.format < 4 ? 2 : 4;
        const bool direct = position.mode == AttributeMode::direct;
        MemoryArrayView array;
        std::uint32_t stride = 0;
        if (!direct) {
            array = memory.array_view(state.cp(0xa0));
            stride = state.cp(0xb0) & 255;
            if (!array.bytes.data || (array.native_unit && !array.host_order && array.native_unit != size)) return false;
        }
        const bool native = array.host_order ? size > 1 : array.native_unit != 0;
        const float scale = fraction_scale(position.format < 2 && !draw.layout.byte_dequant ? 0 : position.fraction_bits);
        const auto component = [&](const std::uint8_t* b) {
            switch (position.format) {
            case 0: return b[0] * scale;
            case 1: return static_cast<std::int8_t>(b[0]) * scale;
            case 2: case 3: {
                std::uint16_t word;
                if (native) std::memcpy(&word, b, 2);
                else word = static_cast<std::uint16_t>(b[0] << 8 | b[1]);
                return (position.format == 3 ? int{static_cast<std::int16_t>(word)} : int{word}) * scale;
            }
            default: {
                std::uint32_t word;
                if (native) std::memcpy(&word, b, 4);
                else word = std::uint32_t{b[0]} << 24 | std::uint32_t{b[1]} << 16 | std::uint32_t{b[2]} << 8 | b[3];
                float value;
                std::memcpy(&value, &word, 4);
                return value;
            }
            }
        };
        const std::size_t bytes = std::size_t{size} * position.components;
        // Two-component positions have z = 0.
        const float z = position.components == 3 ? INFINITY : 0.0f;
        std::array<float, 6> box{{INFINITY, INFINITY, z, -INFINITY, -INFINITY, -z}};
        for (std::size_t v = 0; v < draw.vertex_count; ++v) {
            const auto* data = draw.vertices.data + v * draw.layout.stride;
            if (!direct) {
                const bool short_index = position.mode == AttributeMode::index16;
                const unsigned element = short_index ? unsigned{data[0]} << 8 | data[1] : data[0];
                if (element == (short_index ? 0xffffu : 0xffu)) return false;
                const std::uint64_t at = std::uint64_t{element} * stride;
                if (at > array.bytes.size || bytes > array.bytes.size - at || (native && (at & (size - 1)))) return false;
                data = array.bytes.data + at;
            }
            for (unsigned k = 0; k < position.components; ++k) {
                const float value = component(data + size * k);
                box[k] = std::min(box[k], value);
                box[3 + k] = std::max(box[3 + k], value);
            }
        }
        return outside_clip(state, row, box);
    }
    // Small draws stream eye-space positions with their TEV registers so that
    // consecutive ones differing only in those merge into a single GL draw.
    static bool line_point(const Draw& draw) { return draw.primitive == 0xa8 || draw.primitive == 0xb0 || draw.primitive == 0xb8; }
    // The setup unit's size and texture offsets for a line or point draw. An offset is
    // applied to the input coordinate through the inverse of an affine texgen.
    static LinePointSetup line_point_setup(const State& state, const DirectProgram& plan, std::uint8_t primitive) {
        LinePointSetup setup;
        const auto value = [&](std::uint16_t address) { const auto word = state.xf(address); float f; std::memcpy(&f, &word, 4); return f; };
        for (unsigned i = 0; i < 6; ++i) setup.projection[i] = value(static_cast<std::uint16_t>(0x1020 + i));
        setup.orthographic = state.xf(0x1026) != 0;
        setup.viewport_x = value(0x101a);
        setup.viewport_y = value(0x101b);
        const bool points = primitive == 0xb8;
        const auto sizes = state.bp(0x22);
        setup.size = static_cast<float>((sizes >> (points ? 8 : 0)) & 255) / 6;
        const float amount = line_point_texture_offset((sizes >> (points ? 19 : 16)) & 7);
        if (!amount) return setup;
        const auto& transform = plan.transform;
        std::array<bool, 8> used{};
        for (unsigned i = 0; i < transform.texgen_count; ++i) {
            if (!((state.bp(static_cast<std::uint8_t>(0x30 + 2 * i)) >> (points ? 19 : 18)) & 1)) continue;
            const auto& g = transform.texgens[i];
            if (g.type != 0 || g.source_row < 5 || g.source_row > 12 || g.projected || g.normalize || g.input_abc1)
                throw std::runtime_error("GX line and point texture offsets need an affine texgen from a texture coordinate");
            float m[8], q[12];
            std::memcpy(m, state.xf_data() + state.matrix_index(static_cast<std::uint8_t>(1 + i)) * 4, sizeof m);
            float a = m[0], b = m[1], c = m[4], d = m[5];
            if (transform.dual_texture) {
                std::memcpy(q, state.xf_data() + 0x500 + g.post_row * 4, sizeof q);
                if (q[8] || q[9]) throw std::runtime_error("GX line and point texture offsets need an affine post-transform");
                const float pa = q[0] * a + q[1] * c, pb = q[0] * b + q[1] * d, pc = q[4] * a + q[5] * c, pd = q[4] * b + q[5] * d;
                a = pa; b = pb; c = pc; d = pd;
            }
            const float det = a * d - b * c;
            if (!std::isfinite(det) || !det) throw std::runtime_error("GX line and point texture offsets need an invertible texgen");
            const std::array<float, 4> offset{{d * amount / det, -c * amount / det, -b * amount / det, a * amount / det}};
            const unsigned input = g.source_row - 5u;
            if (used[input] && setup.offsets[input] != offset)
                throw std::runtime_error("GX line and point texture offsets disagree on a shared input coordinate");
            used[input] = true;
            setup.offsets[input] = offset;
        }
        return setup;
    }
    bool draw_batched(Program& base, const State& state, const Draw& draw, MemoryReader& memory, TextureCmprModel cmpr) {
        const auto format = stream_format(draw.layout);
        const bool expanded = line_point(draw);
        if (draw.merged() || (!expanded && draw.vertex_count > 64) || format.normal_vectors || format.matrices) return false;
        const unsigned pn = state.matrix_index(0);
        if (pn > 61) return false;
        const auto& transform = base.plan->transform;
        for (unsigned i = 0; i < transform.texgen_count; ++i) {
            if (transform.texgens[i].type >= 2) continue;
            const unsigned row = state.matrix_index(static_cast<std::uint8_t>(1 + i));
            if (row + 3 > pn && row < pn + 3) return false;
        }
        Program* p = batched_program(base, state, draw);
        if (!p) return false;
        const auto setup = expanded ? line_point_setup(state, *p->plan, draw.primitive) : LinePointSetup{};
        if (limits.verify_textures || !(batch.valid && batch.program == p && batch.bp == state.bp_state_revision() &&
              batch.xf == state.xf_register_revision() && batch.segment == segment && batch.frame == frame &&
              batch.expanded == expanded)) {
            states.emplace_back();
            const auto status = plan_direct_state(state, *p->plan, cmpr, states.back(), limits.guard_band,
                                                   limits.fractional_viewport);
            if (!status) { states.pop_back(); throw std::runtime_error(status.message); }
            // Culling applies to triangles only; the quads drawn for lines and points keep every face.
            if (expanded) states.back().raster.cull = 0;
            ++stats.state_plans;
            std::array<GLuint, 8> names{}, samplers_used{};
            const auto& planned = states.back();
            for (unsigned map = 0; map < 8; ++map) if (p->plan->maps & (1u << map)) {
                names[map] = texture(state, map, planned.textures[map], memory, cmpr);
                samplers_used[map] = sampler(planned.textures[map].sampler);
            }
            batch = {true, state.bp_state_revision(), state.xf_register_revision(), segment, frame, p,
                     static_cast<std::uint32_t>(states.size() - 1), names, samplers_used, expanded};
        }
        // One XF memory write confined to the position rows the block zeroes keeps it valid.
        const auto write = state.last_xf_memory_write();
        if (batch_uniform.valid && batch_uniform.pn == pn && write.revision == state.xf_memory_revision() &&
            batch_uniform.memory + 1 == write.revision && write.first >= pn * 4 && write.end <= pn * 4 + 12)
            batch_uniform.memory = write.revision;
        if (!(batch_uniform.valid && batch_uniform.memory == state.xf_memory_revision() &&
              batch_uniform.registers == state.xf_register_revision() && batch_uniform.segment == segment)) {
            TransformUniforms block;
            direct_transform_uniforms(state, block);
            // Batched programs never read the draw's own position matrix.
            for (unsigned row = pn; row < pn + 3; ++row) block.position[row] = {};
            if (!(batch_uniform.valid && batch_uniform.segment == segment && !std::memcmp(&block, &batch_block, sizeof block))) {
                const auto offset = (uniforms.size() + uniform_alignment - 1) / uniform_alignment * uniform_alignment;
                uniforms.resize(offset + sizeof block);
                std::memcpy(uniforms.data() + offset, &block, sizeof block);
                batch_block = block;
                batch_uniform.offset = static_cast<std::uint32_t>(offset);
                ++stats.uniform_blocks;
            }
            batch_uniform.valid = true; batch_uniform.memory = state.xf_memory_revision();
            batch_uniform.registers = state.xf_register_revision(); batch_uniform.segment = segment; batch_uniform.pn = pn;
        }
        auto batched = format;
        batched.registers = true;
        batched.stride = static_cast<std::uint16_t>(format.stride + 32);
        if (runs.empty() || runs.back().format != batched) {
            const auto base_offset = (vertices.size() + 15) & ~std::size_t{15};
            vertices.resize(base_offset);
            runs.push_back({batched, static_cast<std::uint32_t>(base_offset), 0});
        }
        auto& run = runs.back();
        const auto low = run.vertices;
        batch_vertices.clear();
        const auto result = expanded ? append_stream_records(state, draw, memory, format, batch_vertices)
                                     : append_stream_vertices(state, draw, memory, format, batch_vertices, indices, run.vertices, nullptr);
        if (!result) throw std::runtime_error(result.message);
        const auto registers = capture_tev_uniforms(state).registers;
        static_assert(sizeof registers == 32, "four signed RGBA registers");
        float m[12];
        std::memcpy(m, state.xf_data() + pn * 4, sizeof m);
        // Line and point records are expanded into quads after the eye transform.
        auto& target = expanded ? expansion : vertices;
        const auto at = expanded ? 0 : vertices.size();
        target.resize(at + std::size_t{result.vertices} * batched.stride);
        for (std::uint32_t v = 0; v < result.vertices; ++v) {
            auto* out = target.data() + at + std::size_t{v} * batched.stride;
            std::memcpy(out, batch_vertices.data() + std::size_t{v} * format.stride, format.stride);
            float position[3];
            std::memcpy(position, out, sizeof position);
            float eye[3];
            for (unsigned k = 0; k < 3; ++k)
                eye[k] = m[k * 4] * position[0] + m[k * 4 + 1] * position[1] + m[k * 4 + 2] * position[2] + m[k * 4 + 3];
            std::memcpy(out, eye, sizeof eye);
            std::memcpy(out + format.stride, registers.data(), sizeof registers);
        }
        auto added = result.vertices;
        auto added_indices = result.indices;
        const auto vertex_end = vertices.size();
        if (expanded) {
            const auto quads = expand_lines_points(draw.primitive, expansion.data(), result.vertices, batched, setup,
                                                   vertices, indices, run.vertices);
            added = quads.vertices;
            added_indices = quads.indices;
        }
        const auto first = expanded ? vertex_end : at;
        static constexpr float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        if (added_indices && outside_clip(state, identity, bounds(vertices.data() + first, added, batched.stride))) {
            vertices.resize(first);
            indices.resize(indices.size() - added_indices);
            ++stats.culled_draws;
            warm(*p);
            return true;
        }
        run.vertices += added;
        stats.vertex_bytes += std::size_t{added} * batched.stride;
        ++stats.draws; ++stats.batched_draws;
        auto latches = stream_matrix_latches(state);
        latches[0] &= ~0xffu; // The PN row is unused.
        if (added_indices) push_record(*p, nullptr, static_cast<std::uint32_t>(indices.size() - added_indices), added_indices,
                                       low, low + added - 1, latches, batch.state, batch_uniform.offset,
                                       batch.textures, batch.samplers);
        check_segment();
        return true;
    }
    void draw(const State& state, const Draw& draw, MemoryReader& memory, TextureCmprModel cmpr) {
        // The XF receives every vertex, including those of draws culled or skipped below.
        if (draw.layout.normal.mode != AttributeMode::none) {
            auto vectors = latch;
            if (const auto count = latest_normal(state, draw, memory, vectors)) {
                if (vectors != latch) { latch = vectors; ++latch_revision; }
                latch_components = std::max<std::uint8_t>(latch_components, static_cast<std::uint8_t>(count));
            }
        }
        if (zero_area(draw)) { ++stats.zero_area; return; }
        if (!line_point(draw) && early_outside_clip(state, draw, memory)) { ++stats.culled_draws; return; }
        auto& p = program(state, draw);
        if (line_point(draw)) {
            if (!p.batchable || !draw_batched(p, state, draw, memory, cmpr))
                throw std::runtime_error("GX lines and points need a fast unlit program without per-vertex matrices");
            return;
        }
        if (p.batchable && draw_batched(p, state, draw, memory, cmpr)) return;
        const auto format = stream_format(draw.layout);
        // Resident geometry is found first, so draws outside the clip volume skip all state work.
        Geometry* cached = nullptr;
        std::uint64_t hash = 0;
        bool cache = false;
        if (limits.geometries && cacheable(draw)) {
            build_geometry_key(state, draw, format);
            cached = find_geometry_by_pointer(draw, memory);
            cache = cached || !unstable(hash = geometry_hash(draw));
            if (cache && !cached) cached = find_geometry(draw, memory, hash);
            if (cached && cached->boxed && outside_clip(state, state.matrix_index(0), cached->box)) { ++stats.culled_draws; warm(p); return; }
        }
        if (last_state.valid && last_state.bp != state.bp_revision() && last_state.xf == state.xf_register_revision() &&
            last_state.program == &p && last_state.segment == segment && !limits.verify_textures && same_bp_values(state)) {
            if (last_textures.valid && last_textures.bp == last_state.bp && last_textures.program == &p && same_palettes(state, p))
                last_textures.bp = state.bp_revision();
            last_state.bp = state.bp_revision();
        }
        if (!(last_state.valid && last_state.bp == state.bp_revision() && last_state.xf == state.xf_register_revision() &&
              last_state.program == &p && last_state.segment == segment)) {
            states.emplace_back();
            const auto status = plan_direct_state(state, *p.plan, cmpr, states.back(), limits.guard_band,
                                                   limits.fractional_viewport);
            if (!status) { states.pop_back(); throw std::runtime_error(status.message); }
            last_state = {true, state.bp_revision(), state.xf_register_revision(), segment, &p, static_cast<std::uint32_t>(states.size() - 1)};
            for (unsigned a = 0; a < 256; ++a) planned_bp[a] = state.bp(static_cast<std::uint8_t>(a));
            ++stats.state_plans;
        }
        const std::uint64_t latch_used = p.plan->transform.latched_normal ? latch_revision + 1 : 0;
        const bool uniform_current = last_uniform.valid && last_uniform.memory == state.xf_memory_revision() &&
                                     last_uniform.registers == state.xf_register_revision() && last_uniform.segment == segment &&
                                     last_uniform.latch == latch_used;
        if (!uniform_current) {
            direct_transform_uniforms(state, transform_block);
            if (latch_used)
                for (unsigned k = 0; k < 9; ++k) transform_block.latched_normal[k / 3][k % 3] = latch[k];
        }
        const auto& planned = states[last_state.index];
        if (limits.verify_textures || !(last_textures.valid && last_textures.bp == state.bp_revision() &&
              last_textures.segment == segment && last_textures.frame == frame && last_textures.program == &p)) {
            last_textures.textures.fill(0); last_textures.samplers.fill(0);
            for (unsigned map = 0; map < 8; ++map) if (p.plan->maps & (1u << map)) {
                last_textures.textures[map] = texture(state, map, planned.textures[map], memory, cmpr);
                last_textures.samplers[map] = sampler(planned.textures[map].sampler);
                last_textures.palettes[map] = palette_generation(state, planned.textures[map]);
            }
            last_textures.valid = true; last_textures.bp = state.bp_revision(); last_textures.segment = segment;
            last_textures.frame = frame; last_textures.program = &p;
        }
        const auto latches = format.matrices ? std::array<std::uint32_t, 3>{} : stream_matrix_latches(state);
        // A matrix change alone on resident geometry: an instance of the previous draw.
        if (!uniform_current && cached && cached->clusters.empty()) {
            const GeometryDraw resident{cached->buffer, cached->format, cached->base_vertex, cached->index_offset, cached->short_indices};
            if (join_instances(p, state, draw, resident, cached->index_count, 0, cached->vertex_count - 1, latches)) {
                last_uniform.valid = false;
                return check_segment();
            }
        }
        if (!uniform_current) {
            const auto offset = (uniforms.size() + uniform_alignment - 1) / uniform_alignment * uniform_alignment;
            uniforms.resize(offset + sizeof(TransformUniforms));
            std::memcpy(uniforms.data() + offset, &transform_block, sizeof transform_block);
            last_uniform = {true, state.xf_memory_revision(), state.xf_register_revision(), segment, latch_used,
                            static_cast<std::uint32_t>(offset)};
            ++stats.uniform_blocks;
        }
        if (runs.empty() || runs.back().format != format) {
            const auto base = (vertices.size() + 15) & ~std::size_t{15};
            vertices.resize(base);
            runs.push_back({format, static_cast<std::uint32_t>(base), 0});
        }
        if (cache) {
            Geometry* g = cached;
            if (!g) {
                geometry_vertices.clear(); geometry_indices.clear();
                const auto converted = append_stream_vertices(state, draw, memory, format, geometry_vertices, geometry_indices, 0, &spans);
                if (!converted) throw std::runtime_error(converted.message);
                g = converted.indices ? insert_geometry(state, draw, memory, hash, format, converted) : nullptr;
                if (!g) {
                    // Uncacheable conversion: stream the vertices just produced.
                    // Counted from the buffer: insert_geometry may have merged identical vertices.
                    auto& run = runs.back();
                    const auto low = run.vertices, first = static_cast<std::uint32_t>(indices.size());
                    const auto count = static_cast<std::uint32_t>(geometry_vertices.size() / format.stride);
                    vertices.insert(vertices.end(), geometry_vertices.begin(), geometry_vertices.end());
                    for (auto i : geometry_indices) indices.push_back(i + low);
                    run.vertices += count;
                    stats.vertex_bytes += std::size_t{count} * format.stride;
                    ++stats.draws;
                    if (converted.indices) push_record(p, nullptr, first, converted.indices, low, low + count - 1, latches,
                                                       last_state.index, last_uniform.offset, last_textures.textures, last_textures.samplers);
                    return check_segment();
                }
            }
            ++stats.draws;
            GeometryDraw resident{g->buffer, g->format, g->base_vertex, g->index_offset, g->short_indices};
            // Exact vertex bounds: drivers may shade the whole declared range for each draw.
            auto push = [&](std::uint32_t first, std::uint32_t count, std::uint32_t low, std::uint32_t high) {
                auto part = resident;
                part.index_offset += first * (g->short_indices ? 2 : 4);
                records.push_back({&p, true, part, last_state.index, last_uniform.offset, 0, 0, count, low, high,
                                   last_textures.textures, last_textures.samplers, latches});
            };
            if (g->clusters.empty()) push(0, g->index_count, 0, g->vertex_count - 1);
            else {
                // Visible runs of clusters; gaps of one cluster are drawn rather than split.
                const unsigned row = state.matrix_index(0);
                std::uint32_t begin = 0, end = 0, gap = 0, low = 0, high = 0;
                std::uint32_t pending_low = ~0u, pending_high = 0; // Culled clusters inside the open run.
                bool open = false;
                for (const auto& cluster : g->clusters) {
                    if (!outside_clip(state, row, cluster.box)) {
                        if (open && gap > 1) { push(begin, end - begin, low, high); open = false; }
                        if (!open) { begin = cluster.first; low = cluster.low; high = cluster.high; }
                        else { low = std::min({low, pending_low, cluster.low}); high = std::max({high, pending_high, cluster.high}); }
                        open = true; end = cluster.first + cluster.count; gap = 0; pending_low = ~0u; pending_high = 0;
                    } else {
                        ++stats.culled_clusters;
                        if (open) { ++gap; pending_low = std::min(pending_low, cluster.low); pending_high = std::max(pending_high, cluster.high); }
                    }
                }
                if (open) push(begin, end - begin, low, high);
            }
            return check_segment();
        }
        auto& run = runs.back();
        const auto low = run.vertices;
        const auto at = vertices.size();
        const auto result = append_stream_vertices(state, draw, memory, format, vertices, indices, run.vertices, nullptr);
        if (!result) throw std::runtime_error(result.message);
        if (result.indices && !format.matrices &&
            outside_clip(state, state.matrix_index(0), bounds(vertices.data() + at, result.vertices, format.stride))) {
            vertices.resize(at);
            indices.resize(indices.size() - result.indices);
            ++stats.culled_draws;
            warm(p);
            return check_segment();
        }
        run.vertices += result.vertices;
        stats.vertex_bytes += std::size_t{result.vertices} * format.stride;
        ++stats.draws;
        if (result.indices) push_record(p, nullptr, static_cast<std::uint32_t>(indices.size() - result.indices), result.indices,
                                        low, low + result.vertices - 1, latches, last_state.index, last_uniform.offset,
                                        last_textures.textures, last_textures.samplers);
        check_segment();
    }
    void push_record(Program& p, std::nullptr_t, std::uint32_t first, std::uint32_t count, std::uint32_t low, std::uint32_t high,
                     const std::array<std::uint32_t, 3>& latches, std::uint32_t state, std::uint32_t uniform,
                     const std::array<GLuint, 8>& bound, const std::array<GLuint, 8>& samplers_used) {
        const auto run_index = static_cast<std::uint32_t>(runs.size() - 1);
        if (!records.empty()) {
            auto& last = records.back();
            if (!last.resident && last.program == &p && last.state == state && last.uniform == uniform &&
                last.run == run_index && last.textures == bound && last.samplers == samplers_used &&
                last.latches == latches && last.first + last.count == first) {
                last.count += count;
                last.high = high;
                return;
            }
        }
        records.push_back({&p, false, {}, state, uniform, run_index, first, count, low, high, bound, samplers_used, latches});
    }
    void check_segment() {
        if (vertices.size() + indices.size() * 4 + uniforms.size() > limits.segment_bytes) flush();
    }
    void bind_run(const Run& run) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        bind_format(run.format, run.base);
    }
    void fixed_state(bool depth = true) {
        gl.valid = false;
        efb_framebuffer = depth || !target.color_framebuffer ? target.framebuffer : target.color_framebuffer;
        glBindFramebuffer(GL_FRAMEBUFFER, efb_framebuffer);
        gl.strip = false;
        glBindVertexArray(vao);
        glDisable(GL_DITHER); glDisable(GL_STENCIL_TEST); glDisable(GL_RASTERIZER_DISCARD);
        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE); glDisable(GL_SAMPLE_COVERAGE);
        glEnable(GL_SCISSOR_TEST); glFrontFace(GL_CW); glBlendEquation(GL_FUNC_ADD);
        for (unsigned i = 1; i < 14; ++i) glVertexAttrib4f(i, 0, 0, 0, 0);
    }
    // GL raster state as last set within the current flush; Mali revalidates draw
    // state on every call, so only changed fields are sent.
    GLuint efb_framebuffer = 0;
    struct GlRaster {
        bool valid = false, strip = false;
        RasterRect viewport, scissor;
        std::array<float, 2> depth_range{};
        bool depth_mask = false, depth_test = false, cull = false, blend = false, subtract = false;
        GLenum depth_func = 0, cull_face = 0, source = 0, destination = 0;
        bool blend_alpha = false;
        std::uint8_t alpha_factors = 0;
        std::array<bool, 2> color_mask{};
    } gl;
    static bool same(const RasterRect& a, const RasterRect& b) {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    }
    void apply_raster(const DirectDrawState& s) {
        const auto& r = s.raster;
        const bool fresh = !gl.valid;
        gl.valid = true;
        if (gl.strip != r.strip) {
            if (r.strip && !target.strip_framebuffer) throw std::runtime_error("direct renderer has no EFB strip target");
            glBindFramebuffer(GL_FRAMEBUFFER, r.strip ? target.strip_framebuffer : efb_framebuffer);
            gl.strip = r.strip;
        }
        if (fresh || !same(gl.viewport, r.viewport)) { glViewport(r.viewport.x, r.viewport.y, r.viewport.width, r.viewport.height); gl.viewport = r.viewport; }
        if (fresh || !same(gl.scissor, r.scissor)) { glScissor(r.scissor.x, r.scissor.y, r.scissor.width, r.scissor.height); gl.scissor = r.scissor; }
        if (fresh || gl.depth_range != r.depth_range) { glDepthRangef(r.depth_range[0], r.depth_range[1]); gl.depth_range = r.depth_range; }
        if (fresh || gl.depth_mask != r.depth_write) { glDepthMask(r.depth_write); gl.depth_mask = r.depth_write; }
        if (fresh || gl.depth_test != r.depth_test) { if (r.depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST); gl.depth_test = r.depth_test; }
        if (r.depth_test) {
            constexpr GLenum comparison[] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
            const auto func = comparison[r.depth_compare];
            if (fresh || gl.depth_func != func) { glDepthFunc(func); gl.depth_func = func; }
        }
        const bool cull = r.cull != 0;
        if (fresh || gl.cull != cull) { if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE); gl.cull = cull; }
        if (cull) {
            const GLenum face = r.cull == 1 ? GL_BACK : r.cull == 2 ? GL_FRONT : GL_FRONT_AND_BACK;
            if (fresh || gl.cull_face != face) { glCullFace(face); gl.cull_face = face; }
        }
        const std::array<bool, 2> mask{{r.color_write, s.alpha_write}};
        if (fresh || gl.color_mask != mask) { glColorMask(mask[0], mask[0], mask[0], mask[1]); gl.color_mask = mask; }
        if (fresh || gl.blend != r.blend) { if (r.blend) glEnable(GL_BLEND); else glDisable(GL_BLEND); gl.blend = r.blend; }
        if (r.blend) {
            if (fresh || gl.subtract != r.subtract || gl.alpha_factors != r.alpha_factors) {
                const GLenum colour = r.subtract ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD;
                glBlendEquationSeparate(colour, r.alpha_factors ? GL_FUNC_ADD : colour);
                gl.subtract = r.subtract;
            }
            const auto source = factor(r.source_factor, true), destination = factor(r.destination_factor, false);
            if (fresh || gl.source != source || gl.destination != destination || gl.blend_alpha != r.blend_alpha ||
                gl.alpha_factors != r.alpha_factors) {
                if (r.alpha_factors) glBlendFuncSeparate(source, destination, r.alpha_factors == 1 ? GL_ONE : GL_ZERO, GL_ZERO);
                else if (r.blend_alpha) glBlendFunc(source, destination);
                else glBlendFuncSeparate(source, destination, GL_ONE, GL_ZERO);
                gl.source = source; gl.destination = destination; gl.blend_alpha = r.blend_alpha; gl.alpha_factors = r.alpha_factors;
            }
        }
    }
    static void apply_fast_uniforms(Program& p, const DirectDrawState& s) {
        for (unsigned i = 0; i < 4; ++i) {
            const auto& r = s.tev.registers[i]; const auto& k = s.tev.konst[i];
            if (p.registers[i].update(r)) glUniform4f(p.registers[i].location, r[0] / 255.0f, r[1] / 255.0f, r[2] / 255.0f, r[3] / 255.0f);
            if (p.konst[i].update(k)) glUniform4f(p.konst[i].location, k[0] / 255.0f, k[1] / 255.0f, k[2] / 255.0f, k[3] / 255.0f);
        }
        if (p.alpha.update(s.tev.alpha_reference)) glUniform2i(p.alpha.location, s.tev.alpha_reference[0], s.tev.alpha_reference[1]);
        if (p.plan->z_texture.op && p.z_bias.update({{s.tev.z_texture_bias}})) glUniform1ui(p.z_bias.location, s.tev.z_texture_bias);
        if (p.plan->constant_alpha && p.constant_alpha.update(s.constant_alpha))
            glUniform3f(p.constant_alpha.location, s.constant_alpha[0], s.constant_alpha[1], s.constant_alpha[2]);
        if (p.plan->fog & 7) {
            const auto& fog = s.raster.fog;
            if (p.fog.update(fog.parameters)) glUniform4fv(p.fog.location, 1, fog.parameters.data());
            if (p.fog_color.update(fog.color)) glUniform3fv(p.fog_color.location, 1, fog.color.data());
            if (p.fog_center.update({{fog.center}})) glUniform1f(p.fog_center.location, fog.center);
            if (p.fog_table.update(fog.table)) glUniform1fv(p.fog_table.location, 10, fog.table.data());
        }
        for (unsigned i = 0; i < 16; ++i) if (p.scales[i].update(s.coordinate_scale[i]))
            glUniform2f(p.scales[i].location, s.coordinate_scale[i][0], s.coordinate_scale[i][1]);
        if (p.plan->indirect.lookup_mask) {
            for (unsigned i = 0; i < 16; ++i) if (p.image_sizes[i].update(s.image_size[i]))
                glUniform2f(p.image_sizes[i].location, s.image_size[i][0], s.image_size[i][1]);
            for (unsigned i = 0; i < 4; ++i) if (p.indirect_scales[i].update(s.indirect_scale[i]))
                glUniform4f(p.indirect_scales[i].location, s.indirect_scale[i][0], s.indirect_scale[i][1], s.indirect_scale[i][2], s.indirect_scale[i][3]);
            for (unsigned i = 0; i < 6; ++i) if (p.indirect_rows[i].update(s.indirect_rows[i]))
                glUniform4i(p.indirect_rows[i].location, s.indirect_rows[i][0], s.indirect_rows[i][1], s.indirect_rows[i][2], s.indirect_rows[i][3]);
        }
        for (unsigned map = 0; map < 8; ++map) if ((p.plan->maps & (1u << map)) && p.bias[map].update({{s.sampler_lod[map][0]}}))
            glUniform1f(p.bias[map].location, s.sampler_lod[map][0]);
    }
    static void apply_uniforms(Program& p, const DirectDrawState& s) {
        if (p.plan->fast) return apply_fast_uniforms(p, s);
        for (unsigned i = 0; i < 4; ++i) {
            const auto& r = s.tev.registers[i]; const auto& k = s.tev.konst[i];
            if (p.registers[i].update(r)) glUniform4i(p.registers[i].location, r[0], r[1], r[2], r[3]);
            if (p.konst[i].update(k)) glUniform4i(p.konst[i].location, k[0], k[1], k[2], k[3]);
        }
        if (p.plan->rgba6 && p.pixel.update(s.pixel_controls))
            glUniform4ui(p.pixel.location, s.pixel_controls[0], s.pixel_controls[1], s.pixel_controls[2], s.pixel_controls[3]);
        if (p.alpha.update(s.tev.alpha_reference)) glUniform2i(p.alpha.location, s.tev.alpha_reference[0], s.tev.alpha_reference[1]);
        if (p.plan->z_texture.op && p.z_bias.update({{s.tev.z_texture_bias}})) glUniform1ui(p.z_bias.location, s.tev.z_texture_bias);
        for (unsigned i = 0; i < 16; ++i) if (p.scales[i].update(s.coordinate_scale[i]))
            glUniform2f(p.scales[i].location, s.coordinate_scale[i][0], s.coordinate_scale[i][1]);
        if (p.plan->indirect.lookup_mask) {
            for (unsigned i = 0; i < 16; ++i) if (p.image_sizes[i].update(s.image_size[i]))
                glUniform2f(p.image_sizes[i].location, s.image_size[i][0], s.image_size[i][1]);
            for (unsigned i = 0; i < 4; ++i) if (p.indirect_scales[i].update(s.indirect_scale[i]))
                glUniform4f(p.indirect_scales[i].location, s.indirect_scale[i][0], s.indirect_scale[i][1], s.indirect_scale[i][2], s.indirect_scale[i][3]);
            for (unsigned i = 0; i < 6; ++i) if (p.indirect_rows[i].update(s.indirect_rows[i]))
                glUniform4i(p.indirect_rows[i].location, s.indirect_rows[i][0], s.indirect_rows[i][1], s.indirect_rows[i][2], s.indirect_rows[i][3]);
        }
        for (unsigned map = 0; map < 8; ++map) if ((p.plan->maps & (1u << map)) && p.sampler_lod[map].update(s.sampler_lod[map]))
            glUniform4f(p.sampler_lod[map].location, s.sampler_lod[map][0], s.sampler_lod[map][1], s.sampler_lod[map][2], s.sampler_lod[map][3]);
    }
    void flush() {
        if (records.empty() && pending.empty() && retiring.empty() && warming.empty()) { reset_segment(); return; }
        std::shared_ptr<Segment> segment_data;
        {
            std::lock_guard<std::mutex> lock(spare_mutex);
            if (!spare_segments.empty()) { segment_data = std::move(spare_segments.back()); spare_segments.pop_back(); }
        }
        if (!segment_data) segment_data = std::make_shared<Segment>();
        auto& seg = *segment_data;
        seg.before.swap(pending);
        seg.warm.swap(warming);
        seg.records.swap(records); seg.states.swap(states); seg.runs.swap(runs);
        seg.vertices.swap(vertices); seg.uniforms.swap(uniforms); seg.indices.swap(indices);
        reset_segment();
        seg.after.swap(retiring);
        run_gl([this, segment_data] { execute(segment_data); }, false);
    }
    // GL thread: creations, then the segment's draws, then deletions.
    void execute(const std::shared_ptr<Segment>& segment_data) {
        auto& seg = *segment_data;
        for (auto& op : seg.before) op();
        if (!seg.records.empty()) draw_segment(seg);
        // After the visible draws, so their programs reach the builder first.
        for (Program* p : seg.warm)
            if (!p->name && !p->rejected && builder && builder->running()) ready(*p);
        for (auto& op : seg.after) op();
        seg.before.clear(); seg.after.clear(); seg.warm.clear(); seg.records.clear(); seg.states.clear(); seg.runs.clear();
        seg.vertices.clear(); seg.uniforms.clear(); seg.indices.clear();
        std::lock_guard<std::mutex> lock(spare_mutex);
        if (spare_segments.size() < 4) spare_segments.push_back(segment_data);
    }
    void draw_segment(const Segment& seg) {
        ++gl_segments;
        const auto& records = seg.records; const auto& states = seg.states; const auto& runs = seg.runs;
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(seg.vertices.size()), seg.vertices.data(), GL_STREAM_DRAW);
        glBindVertexArray(vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(seg.indices.size() * 4), seg.indices.data(), GL_STREAM_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(seg.uniforms.size()), seg.uniforms.data(), GL_STREAM_DRAW);
        const bool depth = std::any_of(records.begin(), records.end(), [&](const Record& r) {
            const auto& raster = states[r.state].raster;
            return raster.depth_test && (raster.depth_compare != 7 || raster.depth_write);
        });
        fixed_state(depth);
        const Program* program = nullptr;
        GLuint bound_vao = vao;
        std::uint32_t run = ~0u, state = ~0u, uniform = ~0u;
        std::array<GLuint, 8> bound{}, bound_samplers{};
        std::array<std::uint32_t, 3> latches{{~0u, ~0u, ~0u}};
        bool fresh = true;
        for (auto& r : records) {
            if (!r.program->plan->fast && target.format(r.program->plan->rgba6)) { fixed_state(depth); fresh = true; }
            if (fresh) {
                program = nullptr; run = state = uniform = ~0u; latches = {{~0u, ~0u, ~0u}}; bound_vao = vao;
                for (unsigned i = 0; i < 8; ++i) { bound[i] = ~0u; bound_samplers[i] = ~0u; }
                fresh = false;
            }
            if (!r.program->name && !ready(*r.program)) { ++unready_draws; continue; }
            if (program != r.program) {
                glUseProgram(r.program->name); program = r.program;
            }
            const auto vertex_array = r.resident ? geometry_vao(r.geometry.buffer, r.geometry.format) : vao;
            if (bound_vao != vertex_array) { glBindVertexArray(vertex_array); bound_vao = vertex_array; }
            if (!r.resident && run != r.run) { bind_run(runs[r.run]); run = r.run; }
            const auto& format = r.resident ? r.geometry.format : runs[r.run].format;
            if (!format.matrices && latches != r.latches) {
                glVertexAttribI4ui(14, r.latches[0], r.latches[1], r.latches[2], 0); latches = r.latches;
            }
            if (uniform != r.uniform) {
                glBindBufferRange(GL_UNIFORM_BUFFER, 0, ubo, r.uniform, sizeof(TransformUniforms)); uniform = r.uniform;
            }
            if (r.instances)
                glBindBufferRange(GL_UNIFORM_BUFFER, 1, ubo, r.instance_offset,
                                  static_cast<GLsizeiptr>(r.instances) * transform_instance_rows * 16);
            const auto& s = states[r.state];
            if (state != r.state) { apply_raster(s); state = r.state; }
            apply_uniforms(*r.program, s);
            for (unsigned map = 0; map < 8; ++map) {
                if (!(r.program->plan->maps & (1u << map))) continue;
                if (bound[map] != r.textures[map]) {
                    glActiveTexture(GL_TEXTURE0 + map); glBindTexture(GL_TEXTURE_2D, r.textures[map]); bound[map] = r.textures[map];
                }
                if (bound_samplers[map] != r.samplers[map]) { glBindSampler(map, r.samplers[map]); bound_samplers[map] = r.samplers[map]; }
            }
            // Explicit bounds spare drivers an index scan to find the vertex range.
            if (r.resident && r.instances) {
                const auto& g = r.geometry;
                glDrawElementsInstancedBaseVertex(GL_TRIANGLES, static_cast<GLsizei>(r.count),
                    g.short_indices ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(std::uintptr_t{g.index_offset}), static_cast<GLsizei>(r.instances), g.base_vertex);
            } else if (r.resident) {
                const auto& g = r.geometry;
                glDrawRangeElementsBaseVertex(GL_TRIANGLES, r.low, r.high, static_cast<GLsizei>(r.count),
                    g.short_indices ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(std::uintptr_t{g.index_offset}), g.base_vertex);
            } else {
                glDrawRangeElements(GL_TRIANGLES, r.low, r.high, static_cast<GLsizei>(r.count), GL_UNSIGNED_INT,
                                    reinterpret_cast<const void*>(std::uintptr_t{r.first} * 4));
            }
            ++stats.submissions;
        }
        for (unsigned map = 0; map < 8; ++map) glBindSampler(map, 0);
        glBindVertexArray(0);
        if (gl.strip) { glBindFramebuffer(GL_FRAMEBUFFER, efb_framebuffer); gl.strip = false; }
        gl_check("direct GX segment");
        ++stats.segments;
    }
    void reset_segment() {
        vertices.clear(); indices.clear(); uniforms.clear(); records.clear(); states.clear(); runs.clear();
        for (auto& g : retired_geometries) delete_geometry(g);
        retired_geometries.clear();
        ++segment;
        evict_geometry();
        last_state.valid = last_uniform.valid = batch_uniform.valid = false;
        instance_group.open = false;
        textures_changed();
        evict_textures();
        evict_programs(nullptr);
    }
};

GlesDirect::GlesDirect(GlesDirectLimits limits, FramebufferFetch fetch, Target target)
    : impl_(std::make_unique<Impl>(limits, fetch, std::move(target))) {}
GlesDirect::~GlesDirect() = default;
void GlesDirect::draw(const State& state, const Draw& draw, MemoryReader& memory, TextureCmprModel cmpr) {
    impl_->draw(state, draw, memory, cmpr);
}
void GlesDirect::flush() { impl_->flush(); }
void GlesDirect::next_frame() { ++impl_->frame; impl_->textures_changed(); impl_->pack_loose(); }
void GlesDirect::invalidate(const void* data, std::size_t size) {
    impl_->invalidate_pointer(static_cast<const std::uint8_t*>(data), size);
}
GlesDirect::ResidentTarget GlesDirect::resident_copy(const TextureCopy& p, std::uint32_t token, std::uint8_t* destination, std::size_t capacity) {
    return impl_->resident_copy(p, token, destination, capacity);
}
void GlesDirect::release(std::uint32_t token, std::size_t bytes) { impl_->release(token, bytes); }
GlesProgramBuilderStats GlesDirect::program_builder() const {
    return impl_->builder ? impl_->builder->stats() : GlesProgramBuilderStats{};
}

bool link_unready_program(bool requested, std::chrono::milliseconds waited, unsigned link_ms, std::chrono::milliseconds since_link,
                          unsigned interval_ms) {
    return requested && link_ms && waited.count() >= std::chrono::milliseconds::rep(link_ms) &&
           since_link.count() >= std::chrono::milliseconds::rep(interval_ms);
}

void GlesDirect::build_programs_in_background() {
    if (impl_->builder) impl_->builder->background();
}

GlesDirectStats GlesDirect::stats() const {
    auto result = impl_->stats;
    result.textures = impl_->textures.size();
    result.geometries = impl_->geometries.size();
    result.programs = impl_->programs.size();
    result.program_link_us = impl_->link_us.load();
    result.unready_draws = impl_->unready_draws.load();
    for (unsigned i = 0; i < 5; ++i) result.program_waits[i] = impl_->program_waits[i].load();
    result.program_wait_max_ms = impl_->program_wait_max_ms.load();
    result.fallback_links = impl_->fallback_links.load();
    result.fallback_link_max_ms = impl_->fallback_link_max_ms.load();
    if (impl_->program_cache) result.program_cache = impl_->program_cache->stats();
    return result;
}

} // namespace starlet::gx
