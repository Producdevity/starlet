#pragma once

#include "starlet/gx/decoder.hpp"
#include "starlet/gx/display_copy.hpp"
#include "starlet/gx/efb_bloom.hpp"
#include "starlet/gx/efb_clear.hpp"
#include "starlet/gx/efb_peek.hpp"
#include "starlet/gx/gles/renderer.hpp"
#include "starlet/gx/texture_copy.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// GX trace files: one recorded stretch of a game's GX work, replayable without the game
// (tools/replay). Little-endian, written and read on the same pointer width.
namespace starlet::gx::trace {

constexpr char magic[8] = {'S', 'T', 'L', 'T', 'R', 'A', 'C', 'E'};
constexpr std::uint32_t version = 1;

// Address spaces of recorded memory. canonical: copy, view and borrow results, and the
// destinations of renderer calls; raw: array_view bytes as stored; xf: copy_indexed_xf results.
enum class Space : std::uint8_t { canonical, raw, xf };

enum class Op : std::uint8_t {
    // Memory, written before the call that read it.
    mem = 1,        // space, address, size, bytes
    fail = 2,       // space, address, size: the read failed
    array = 3,      // address, size, native_unit, host_order: array_view result
    state = 10,     // decoder state the following calls start from
    // Decoder calls, each closed by call_end after the events it caused.
    feed = 20, extend = 21, finish = 22, call_list = 23, inject = 24, texture_extent = 25, load_tlut = 26, recover = 27,
    call_end = 29,  // error, bool result
    // Sink callbacks during a decoder call, each closed by sink_end.
    sink_draw = 30, sink_bp = 31, sink_invalidate = 32, sink_host = 33, sink_filter = 34,
    sink_end = 39,
    // Renderer calls.
    draw_direct = 40, draw = 41, clear = 42, clear_efb = 43, begin_offscreen = 44, end_offscreen = 45,
    begin_bloom = 46, end_bloom = 47, copy_display = 48, copy_texture = 49, copy_known = 50, write_back = 51,
    land_write_backs = 52, release_resident = 53, dma_read = 54, dma_write = 55, wait_until_idle = 56,
    peek_depth = 57, peek_color = 58, snapshot = 59, wait_snapshot = 60, select_snapshot = 61,
    release_snapshot = 62, retain_snapshot = 63, release_snapshots = 64, request_depth = 65, wait_depth = 66,
    release_depth = 67, frames_in_flight = 68, present_into = 69,
    // Frame boundaries.
    present_framebuffer = 70, present = 71, end_frame = 72,
    // Draws of captured packets (GlesRenderer::draw(DrawPacket), draw_batch): not replayable.
    packet_draw = 73,
    end = 99,
};

class Writer {
public:
    explicit Writer(std::vector<std::uint8_t>& out) : out_(out) {}
    void u8(std::uint8_t v) { out_.push_back(v); }
    void u16(std::uint16_t v) { raw(&v, 2); }
    void u32(std::uint32_t v) { raw(&v, 4); }
    void u64(std::uint64_t v) { raw(&v, 8); }
    void i32(std::int32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void op(Op o) { u8(static_cast<std::uint8_t>(o)); }
    void raw(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        out_.insert(out_.end(), p, p + size);
    }
    void bytes(const void* data, std::size_t size) { u64(size); if (size) raw(data, size); }

private:
    std::vector<std::uint8_t>& out_;
};

// Sequential reader over a FILE (a trace file or a decompressing pipe).
class Reader {
public:
    explicit Reader(std::FILE* file) : file_(file) {}
    void raw(void* data, std::size_t size) {
        auto* p = static_cast<std::uint8_t*>(data);
        while (size) {
            if (at_ == end_) fill();
            const std::size_t n = std::min(size, end_ - at_);
            std::memcpy(p, buffer_.data() + at_, n);
            at_ += n; p += n; size -= n; offset_ += n;
        }
    }
    std::uint8_t u8() { std::uint8_t v; raw(&v, 1); return v; }
    std::uint16_t u16() { std::uint16_t v; raw(&v, 2); return v; }
    std::uint32_t u32() { std::uint32_t v; raw(&v, 4); return v; }
    std::uint64_t u64() { std::uint64_t v; raw(&v, 8); return v; }
    std::int32_t i32() { std::int32_t v; raw(&v, 4); return v; }
    float f32() { float v; raw(&v, 4); return v; }
    Op op() { return static_cast<Op>(u8()); }
    std::vector<std::uint8_t> bytes() {
        const auto size = u64();
        if (size > (std::uint64_t{1} << 32)) throw std::runtime_error("trace: implausible byte count");
        std::vector<std::uint8_t> v(size);
        if (size) raw(v.data(), size);
        return v;
    }
    std::uint64_t offset() const { return offset_; }

private:
    void fill() {
        if (buffer_.empty()) buffer_.resize(1 << 20);
        end_ = std::fread(buffer_.data(), 1, buffer_.size(), file_);
        at_ = 0;
        if (!end_) throw std::runtime_error("trace: unexpected end of file");
    }
    std::FILE* file_;
    std::vector<std::uint8_t> buffer_;
    std::size_t at_ = 0, end_ = 0;
    std::uint64_t offset_ = 0;
};

inline void put(Writer& w, const RasterRect& r) { w.i32(r.x); w.i32(r.y); w.i32(r.width); w.i32(r.height); }
inline void get(Reader& r, RasterRect& v) { v.x = r.i32(); v.y = r.i32(); v.width = r.i32(); v.height = r.i32(); }
inline void put(Writer& w, const EfbRegion& r) { w.i32(r.x); w.i32(r.y); w.i32(r.width); w.i32(r.height); }
inline void get(Reader& r, EfbRegion& v) { v.x = r.i32(); v.y = r.i32(); v.width = r.i32(); v.height = r.i32(); }

inline void put(Writer& w, const EfbClear& c) {
    w.i32(c.efb.height);
    put(w, c.rectangle);
    put(w, c.strip_rectangle);
    w.raw(c.tile.data(), sizeof c.tile);
    w.u8(c.color_write); w.u8(c.alpha_write); w.u8(c.rgba6); w.u8(c.depth_write);
    w.u32(c.depth);
}
inline void get(Reader& r, EfbClear& c) {
    c.efb.height = r.i32();
    get(r, c.rectangle);
    get(r, c.strip_rectangle);
    r.raw(c.tile.data(), sizeof c.tile);
    c.color_write = r.u8(); c.alpha_write = r.u8(); c.rgba6 = r.u8(); c.depth_write = r.u8();
    c.depth = r.u32();
}
inline void put(Writer& w, const std::optional<EfbClear>& c) { w.u8(c.has_value()); if (c) put(w, *c); }
inline void get(Reader& r, std::optional<EfbClear>& c) { if (r.u8()) { c.emplace(); get(r, *c); } else c.reset(); }

inline void put(Writer& w, const DisplayCopy& c) {
    w.i32(c.efb.height); w.i32(c.x); w.i32(c.y); w.i32(c.width); w.i32(c.height); w.u64(c.stride);
    for (auto f : c.filter) w.u32(f);
    w.u8(c.clamp_top); w.u8(c.clamp_bottom); w.u8(c.rgba6);
    put(w, c.clear);
}
inline void get(Reader& r, DisplayCopy& c) {
    c.efb.height = r.i32(); c.x = r.i32(); c.y = r.i32(); c.width = r.i32(); c.height = r.i32(); c.stride = r.u64();
    for (auto& f : c.filter) f = r.u32();
    c.clamp_top = r.u8(); c.clamp_bottom = r.u8(); c.rgba6 = r.u8();
    get(r, c.clear);
}

inline void put(Writer& w, const TextureCopy& c) {
    w.i32(c.efb.height); w.i32(c.x); w.i32(c.y); w.i32(c.width); w.i32(c.height); w.u32(c.format); w.u64(c.stride);
    w.u8(c.rgba6); w.u8(c.half_scale); w.u8(c.depth); w.u8(c.luminance);
    for (auto f : c.filter) w.i32(f);
    w.u8(c.clamp_top); w.u8(c.clamp_bottom); w.u8(c.strip); w.u8(c.split);
    put(w, c.clear);
}
inline void get(Reader& r, TextureCopy& c) {
    c.efb.height = r.i32(); c.x = r.i32(); c.y = r.i32(); c.width = r.i32(); c.height = r.i32(); c.format = r.u32(); c.stride = r.u64();
    c.rgba6 = r.u8(); c.half_scale = r.u8(); c.depth = r.u8(); c.luminance = r.u8();
    for (auto& f : c.filter) f = r.i32();
    c.clamp_top = r.u8(); c.clamp_bottom = r.u8(); c.strip = r.u8(); c.split = r.u8();
    get(r, c.clear);
}

inline void put(Writer& w, const EfbBloom& b) {
    w.u8(b.threshold); w.u8(b.level1_weight); w.u8(b.level2_weight); w.u8(b.intensity);
    for (const auto& t : b.level1) { w.f32(t[0]); w.f32(t[1]); }
    for (const auto& t : b.level2) { w.f32(t[0]); w.f32(t[1]); }
}
inline void get(Reader& r, EfbBloom& b) {
    b.threshold = r.u8(); b.level1_weight = r.u8(); b.level2_weight = r.u8(); b.intensity = r.u8();
    for (auto& t : b.level1) { t[0] = r.f32(); t[1] = r.f32(); }
    for (auto& t : b.level2) { t[0] = r.f32(); t[1] = r.f32(); }
}

// The renderer settings a replay needs; program caches and background builds stay off there.
inline void put(Writer& w, const GlesLimits& l) {
    w.i32(l.efb.height); w.i32(l.aspect_width); w.i32(l.aspect_height);
    w.u64(l.texture_bytes); w.u64(l.programs); w.u64(l.textures);
    w.u64(l.geometry.bytes); w.u64(l.geometry.entries);
    const auto& d = l.direct;
    w.u64(d.texture_bytes); w.u64(d.textures); w.u64(d.programs); w.u64(d.segment_bytes);
    w.u64(d.geometry_bytes); w.u64(d.geometries); w.u32(d.geometry_min_vertices);
    w.u8(d.verify_textures); w.u8(static_cast<std::uint8_t>(d.shading)); w.i32(d.snapshot_scale);
    w.u8(static_cast<std::uint8_t>(d.guard_band)); w.u8(static_cast<std::uint8_t>(d.fractional_viewport));
    w.u8(d.stamp_resident_copies); w.u8(d.vertex_fog);
}
inline void get(Reader& r, GlesLimits& l) {
    l.efb.height = r.i32(); l.aspect_width = r.i32(); l.aspect_height = r.i32();
    l.texture_bytes = r.u64(); l.programs = r.u64(); l.textures = r.u64();
    l.geometry.bytes = r.u64(); l.geometry.entries = r.u64();
    auto& d = l.direct;
    d.texture_bytes = r.u64(); d.textures = r.u64(); d.programs = r.u64(); d.segment_bytes = r.u64();
    d.geometry_bytes = r.u64(); d.geometries = r.u64(); d.geometry_min_vertices = r.u32();
    d.verify_textures = r.u8(); d.shading = static_cast<DirectShading>(r.u8()); d.snapshot_scale = r.i32();
    d.guard_band = static_cast<GuardBand>(r.u8()); d.fractional_viewport = static_cast<FractionalViewport>(r.u8());
    d.stamp_resident_copies = r.u8(); d.vertex_fog = r.u8();
}

template <std::size_t N> void put_bits(Writer& w, const std::bitset<N>& b) {
    for (std::size_t i = 0; i < N; i += 64) {
        std::uint64_t word = 0;
        for (std::size_t j = 0; j < 64 && i + j < N; ++j) word |= std::uint64_t{b[i + j]} << j;
        w.u64(word);
    }
}
template <std::size_t N> void get_bits(Reader& r, std::bitset<N>& b) {
    for (std::size_t i = 0; i < N; i += 64) {
        const auto word = r.u64();
        for (std::size_t j = 0; j < 64 && i + j < N; ++j) b[i + j] = word >> j & 1;
    }
}
inline void put(Writer& w, const Limits& l) {
    w.u64(l.max_command_bytes); w.u64(l.max_display_list_bytes); w.u64(l.max_expanded_bytes); w.u64(l.max_commands);
    w.u32(l.max_display_list_depth); w.u8(l.merge_primitives);
    put_bits(w, l.host_opcodes); put_bits(w, l.filtered_bp); put_bits(w, l.dropped_bp);
}
inline void get(Reader& r, Limits& l) {
    l.max_command_bytes = r.u64(); l.max_display_list_bytes = r.u64(); l.max_expanded_bytes = r.u64(); l.max_commands = r.u64();
    l.max_display_list_depth = r.u32(); l.merge_primitives = r.u8();
    get_bits(r, l.host_opcodes); get_bits(r, l.filtered_bp); get_bits(r, l.dropped_bp);
}

// Trace header, after the magic and version.
struct Header {
    GlesLimits renderer;
    Limits decoder;
    EfbLayout efb;
    std::bitset<256> observed_bp;
    std::uint32_t frames = 0;
    std::string source; // Free text: the port and scene.
};
inline void put(Writer& w, const Header& h) {
    put(w, h.renderer); put(w, h.decoder); w.i32(h.efb.height); put_bits(w, h.observed_bp); w.u32(h.frames);
    w.bytes(h.source.data(), h.source.size());
}
inline void get(Reader& r, Header& h) {
    get(r, h.renderer); get(r, h.decoder); h.efb.height = r.i32(); get_bits(r, h.observed_bp); h.frames = r.u32();
    const auto s = r.bytes();
    h.source.assign(s.begin(), s.end());
}

} // namespace starlet::gx::trace

namespace starlet::gx {
// Saves and restores a decoder's register state and partial command (trace/access.cpp).
struct TraceAccess {
    static void save(const Decoder&, trace::Writer&);
    static void load(Decoder&, trace::Reader&);
};
} // namespace starlet::gx
