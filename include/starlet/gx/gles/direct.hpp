#pragma once

#include "starlet/gx/gles/program_builder.hpp"
#include "starlet/gx/gles/program_cache.hpp"
#include "starlet/gx/direct.hpp"
#include "starlet/gx/texture_copy.hpp"

#include <functional>
#include <string>
#include <vector>

namespace starlet::gx {

struct GlesDirectLimits {
    std::size_t texture_bytes = 32 * 1024 * 1024;
    std::size_t textures = 256;
    std::size_t programs = 256;
    // Pending segment payload before an implicit submission.
    std::size_t segment_bytes = 16 * 1024 * 1024;
    // Converted indexed geometry reused while its FIFO bytes and consumed array
    // spans are unchanged; both are compared on every use.
    std::size_t geometry_bytes = 32 * 1024 * 1024;
    std::size_t geometries = 8192;
    // Smallest indexed draw kept as resident geometry; smaller ones stream. Resident draws
    // can be instanced, which suits games that repeat small models (Pikmin 2's title).
    unsigned geometry_min_vertices = 8;
    // Hash texture sources on every use instead of once per frame.
    bool verify_textures = false;
    DirectShading shading = DirectShading::exact;
    // EFB pixels per draw-sync snapshot sample along each axis (1, 2 or 4); 0 keeps
    // quarter resolution under fast shading and full resolution otherwise.
    int snapshot_scale = 0;
    // How scissors reaching past the viewport are drawn; see GuardBand.
    GuardBand guard_band = GuardBand::reject;
    // Whether viewports off the pixel grid are refused or rounded; see FractionalViewport.
    FractionalViewport fractional_viewport = FractionalViewport::reject;
    // Directory for linked program binaries kept across runs; empty disables it.
    // With fast shading, the words of every linked program are added to programs.list there.
    std::string program_cache;
    // Program lists built into the cache in the background; see GlesProgramBuilder.
    std::vector<std::string> program_lists;
    // Fast shading with a cache: programs missing from it are built in the background while
    // their draws are skipped. Otherwise the GL thread compiles them before the draw.
    bool background_programs = true;
    // For destination memory whose writes nobody reports, such as a recompiled game's RAM:
    // resident copies stamp up to 64 32-byte blocks spread over their destination (first
    // and last included) and are sampled or materialized only while every stamp is
    // intact. A game that reuses the memory breaks a stamp, and its bytes then stay.
    bool stamp_resident_copies = false;
};
// An EFB texture copy held only on the GPU: texture holds the texels the copy's
// sample format decodes, so memory at destination is stale until materialized.
struct GlesResidentCopy {
    std::uint32_t token = 0;
    std::size_t bytes = 0;
    unsigned texture = 0;
    unsigned framebuffer = 0; // Renders into texture; attached once, as reattachment revalidates on Mali.
    TextureCopy plan;
    std::uint8_t* destination = nullptr;
    std::size_t capacity = 0;
    // Nonzero when the destination is stamped; see GlesDirectLimits::stamp_resident_copies.
    std::uint64_t stamp = 0;
    void write_stamps() const;
    // Whether the destination's 32-byte block still holds its stamp.
    bool stamped(std::size_t block) const;
    // Unstamped, or every stamped block still holds its stamp.
    bool intact() const;
};
struct GlesDirectStats {
    std::uint64_t draws = 0, zero_area = 0, submissions = 0, segments = 0, vertex_bytes = 0;
    std::uint64_t resident_copies = 0, resident_hits = 0, materialized = 0;
    std::uint64_t geometry_hits = 0, geometry_misses = 0, geometry_validated_bytes = 0;
    std::size_t geometry_bytes = 0, geometries = 0;
    std::uint64_t program_misses = 0, state_plans = 0, uniform_blocks = 0, batched_draws = 0, culled_draws = 0, culled_clusters = 0;
    std::uint64_t texture_uploads = 0, texture_validations = 0, texture_hash_bytes = 0, texture_mutations = 0;
    std::size_t texture_bytes = 0, textures = 0, programs = 0;
    // Time the GL thread spent compiling, linking or loading programs.
    std::uint64_t program_link_us = 0;
    // Draws skipped while the program builder compiled their program.
    std::uint64_t unready_draws = 0;
    // Draws added as instances to the previous resident draw; see the instancing notes in direct.md.
    std::uint64_t instanced_draws = 0;
    // Texture resolutions (cache lookups, validation, decoding) the draws needed.
    std::uint64_t texture_lookups = 0;
    GlesProgramCacheStats program_cache;
};

// Owner-thread direct draw path. Each draw is fully read from State and source
// memory before returning, without GL calls: texture uploads, program links and
// geometry buffers are queued with the draws, and flush() hands the segment to
// Target::execute, which callers invoke before any other EFB access. Texture
// sources are content-hashed once per frame (or on every use when verifying) and
// on overlap with CPU writes reported through invalidate().
class GlesDirect {
public:
    struct Target {
        unsigned framebuffer = 0; // GLuint of the EFB.
        // The EFB color texture alone: segments that never read or write depth render
        // here, so tilers neither load nor store the depth buffer for them.
        unsigned color_framebuffer = 0;
        // Converts the EFB backing to the requested pixel format; true if GL state changed.
        std::function<bool(bool rgba6)> format;
        // Writes a resident copy's exact bytes to its destination memory, after all
        // previously submitted GL work.
        std::function<void(const GlesResidentCopy&)> materialize;
        // Runs GL work on the context's thread after previously submitted work, waiting
        // for it when asked. Unset: inline on the calling thread.
        std::function<void(std::function<void()>, bool wait)> execute;
        // The EFB rows below the displayed frame, for draws whose scissor lies there.
        unsigned strip_framebuffer = 0;
    };
    GlesDirect(GlesDirectLimits, FramebufferFetch, Target);
    ~GlesDirect();
    GlesDirect(const GlesDirect&) = delete;
    GlesDirect& operator=(const GlesDirect&) = delete;
    void draw(const State&, const Draw&, MemoryReader&, TextureCmprModel);
    void flush();
    void next_frame();
    // CPU bytes in [data, data+size) changed; textures decoded from them are stale.
    void invalidate(const void* data, std::size_t size);
    // Registers the copy at token as GPU-resident and returns its destination-sized
    // RGBA8 texture and a framebuffer with it attached, for the caller to render into. Overlapped resident copies are
    // replaced, or materialized first when only partly covered. Call after flush(),
    // and flush() again before rendering into the target: its creation is queued.
    struct ResidentTarget { unsigned texture = 0, framebuffer = 0; };
    ResidentTarget resident_copy(const TextureCopy&, std::uint32_t token, std::uint8_t* destination, std::size_t capacity);
    // The token range no longer names memory; its resident copies are dropped.
    void release(std::uint32_t token, std::size_t bytes);
    GlesDirectStats stats() const;
    // The program builder's progress; not running without one.
    GlesProgramBuilderStats program_builder() const;
    // Ends the builder's start-up phase; see GlesProgramBuilder::background.
    void build_programs_in_background();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace starlet::gx
