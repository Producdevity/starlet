#pragma once

#include "starlet/gx/decoder.hpp"

namespace starlet::gx {

struct ExpandedVertex {
    std::array<float, 3> position{};
    std::array<float, 9> normal{};
    std::array<std::array<std::uint8_t, 4>, 2> colors{};
    std::array<std::array<float, 2>, 8> texcoords{};
    // Effective matrix row indices; explicit attributes override the current XF latches.
    std::array<std::uint8_t, 9> matrix_indices{};
    bool disabled = false;
};

enum class Topology { triangles, lines, points };
enum class VertexError {
    none, invalid_input, invalid_layout, unsupported_layout, invalid_primitive,
    partial_primitive, partially_disabled_primitive, memory_read, storage_limit, read_limit
};

struct VertexLimits {
    std::size_t max_vertices = 65535;
    std::size_t max_indices = 3 * 65535;
    // Object/vector payload storage, excluding allocator bookkeeping and caller storage.
    std::size_t max_storage_bytes = 16 * 1024 * 1024;
    std::size_t max_array_bytes = 16 * 1024 * 1024;
};

class VertexSnapshot;
struct VertexResult;
struct VertexCacheLimits { std::size_t bytes = 64 * 1024 * 1024, entries = 4096; };
struct VertexCacheStats { std::uint64_t hits = 0, misses = 0, evictions = 0, invalidations = 0; std::size_t bytes = 0, entries = 0; };
// Owner-thread cache. Hits verify current FIFO, layout, matrix latches, encoding
// and consumed array spans. Entries own bytes and immutable geometry, never source
// pointers. Payload/entry limits exclude allocator bookkeeping; callers separately
// bound retained snapshots, which can outlive eviction. Unavailable views bypass
// caching; all per-capture limits still apply on hits.
class VertexCache {
public:
    explicit VertexCache(VertexCacheLimits = {});
    ~VertexCache();
    VertexCache(const VertexCache&) = delete;
    VertexCache& operator=(const VertexCache&) = delete;
    VertexCacheStats stats() const;
private:
    friend VertexResult capture_vertices(const State&, const Draw&, MemoryReader&, VertexLimits, VertexCache*);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct VertexResult {
    VertexError error = VertexError::none;
    std::string message;
    std::shared_ptr<const VertexSnapshot> snapshot;
    explicit operator bool() const { return error == VertexError::none; }
};

class VertexSnapshot {
public:
    const VertexLayout& layout() const { return layout_; }
    const std::vector<ExpandedVertex>& vertices() const { return geometry_->vertices; }
    const std::vector<std::uint16_t>& indices() const { return geometry_->indices; }
    Topology topology() const { return topology_; }
    std::uint8_t primitive() const { return primitive_; }
    std::uint8_t vat() const { return vat_; }
    std::uint64_t state_revision() const { return state_revision_; }
    std::size_t storage_bytes() const { return storage_bytes_; }
    std::size_t array_bytes_read() const { return array_bytes_read_; }

private:
    friend VertexResult capture_vertices(const State&, const Draw&, MemoryReader&, VertexLimits, VertexCache*);
    VertexSnapshot() = default;
    VertexLayout layout_;
    struct Geometry {
        std::vector<ExpandedVertex> vertices;
        std::vector<std::uint16_t> indices;
    };
    std::shared_ptr<const Geometry> geometry_;
    Topology topology_ = Topology::triangles;
    std::uint8_t primitive_ = 0;
    std::uint8_t vat_ = 0;
    std::uint64_t state_revision_ = 0;
    std::size_t storage_bytes_ = 0;
    std::size_t array_bytes_read_ = 0;
};

// Call inside CommandSink::draw, before any FIFO, state or array mutation. All source
// memory must remain stable throughout this call. MemoryReader returns canonical
// big-endian GX bytes; native typed arrays require the platform's explicit conversion.
// Scoped array views avoid per-index reader calls; only decoded values are retained.
// This geometry-only snapshot retains immutable expanded attributes and indices.
// It captures effective matrix indices, but no BP/XF registers, matrix floats,
// lighting or texture data.
// Absent attributes are zero placeholders, not emulated hardware defaults. The
// renderer must supply missing attributes and capture its other draw state.
// Failure publishes no snapshot; allocation/MemoryReader exceptions propagate.
// Limits apply per snapshot; a retaining caller must separately bound aggregate storage.
VertexResult capture_vertices(const State& state, const Draw& draw, MemoryReader& memory,
                              VertexLimits limits = {});
VertexResult capture_vertices(const State&, const Draw&, MemoryReader&, VertexLimits, VertexCache*);

} // namespace starlet::gx
