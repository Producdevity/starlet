#pragma once

#include "starlet/gx/draw.hpp"
#include "starlet/gx/gpu_completion.hpp"
#include "starlet/gx/efb_clear.hpp"
#include "starlet/gx/efb_peek.hpp"
#include "starlet/gx/efb_bloom.hpp"
#include "starlet/gx/display_copy.hpp"
#include "starlet/gx/texture_copy.hpp"
#include "starlet/gx/gles/direct.hpp"

#include <functional>

namespace starlet::gx {

struct GlesLimits {
    // The game's EFB frame; draws, copies and clears planned for another layout fail.
    EfbLayout efb;
    // The shape the VI output is presented in: 4:3, a game's 16:9 mode, or a port's wider view.
    int aspect_width = 4, aspect_height = 3;
    std::size_t texture_bytes = 32 * 1024 * 1024;
    std::size_t programs = 128;
    std::size_t textures = 128;
    VertexCacheLimits geometry;
    GlesDirectLimits direct;
};
struct GlesStats {
    std::uint64_t draws = 0, submissions = 0, shader_compiles = 0, texture_uploads = 0;
    std::size_t texture_bytes = 0;
    std::uint64_t display_copies = 0, texture_copies = 0, presentations = 0, format_conversions = 0, late_depth_snapshots = 0, blooms = 0;
    // EFB peeks, those against the live EFB or waiting for a snapshot (both wait for
    // queued GL work), and snapshot requests refused for want of a slot. Region misses:
    // peeks outside a snapshot's read-back region, read from the retained snapshot.
    std::uint64_t peeks = 0, live_peeks = 0, peek_waits = 0, refused_snapshots = 0, region_misses = 0;
    // EFB attachments invalidated before a copy whose clear rewrites them (see renderer.md).
    std::uint64_t invalidations = 0;
    TextureCacheStats capture_cache;
    VertexCacheStats geometry_cache;
    GlesDirectStats direct;
};
// Requires a current GLES3.1 context owned by the constructing thread, kept
// alive through destruction. SDL window/events/swap belong to the frontend.
// Owns a640x456 EFB; present copies it to the requested drawable. This initial
// draw sink admits plan_gles_draw's explicit domain. Native MainLoop clears
// use a separate validated operation. Native display copies and VI presentation
// admit the explicit domains documented in display_copy.md and texture_copy.md.
// This does not establish original-GX numerical precision. Transfer state and
// sampler bindings are set explicitly. A failure after GL mutation latches:
// only stats and destruction remain available, since driver state cannot be
// rolled back reliably. Unsupported packet validation precedes GL mutation.
class GlesRenderer : public GpuCompletionBackend, public EfbClearBackend, public DisplayCopyBackend, public TextureCopyBackend, public EfbPeekBackend,
                     public EfbBloomBackend {
public:
    explicit GlesRenderer(GlesLimits = {});
    ~GlesRenderer();
    GlesRenderer(const GlesRenderer&) = delete;
    GlesRenderer& operator=(const GlesRenderer&) = delete;
    void clear(std::array<std::uint8_t, 3> color);
    void draw(const DrawPacket&);
    // Synchronous, ordered submission of at most 4096 owned snapshots. Validates
    // the entire batch before GL mutation; only fully compatible neighbors merge.
    void draw_batch(const DrawPacket*, std::size_t count);
    // Captures one single-primitive draw into a packet; merged draws are rejected.
    void draw(const State&, const Draw&, MemoryReader&, TextureCmprModel = TextureCmprModel::reject_unverified);
    // Direct path (gles_direct.hpp): reads all draw inputs before returning and defers
    // GL submission until the next EFB access, presentation or idle boundary. The only
    // entry that accepts merged draws (Limits::merge_primitives).
    void draw_direct(const State&, const Draw&, MemoryReader&, TextureCmprModel = TextureCmprModel::reject_unverified);
    void clear_efb(const EfbClear&) override;
    std::uint32_t peek_depth(int x, int y) override;
    std::uint32_t peek_color(int x, int y) override;
    // With the direct path active, draw-sync callbacks are deferred against snapshots
    // of the EFB's packed D24 depth and colour; otherwise snapshot_efb declines.
    int snapshot_efb() override;
    int snapshot_efb(const EfbRegion&) override;
    bool snapshot_ready(int) const override;
    void wait_snapshot(int) override;
    bool peek_depth_snapshot(int index, int x, int y, std::uint32_t& value) const override;
    bool peek_color_snapshot(int index, int x, int y, std::uint32_t& value) const override;
    void select_snapshot(int) override;
    void release_snapshot(int) override;
    // Moves ownership to the calling thread, which must have the GL context current.
    void adopt_thread();
    // Direct path: GL work moves to a submission thread that runs it in call order,
    // while the owner thread keeps recording draws. release() runs here first; bind()
    // and unbind() make the context current on, and release it from, that thread.
    // Presentation then calls the swap function there. stop_submission() submits
    // pending draws and joins; the owner must rebind the context afterwards.
    void start_submission(std::function<void()> release, std::function<void()> bind, std::function<void()> unbind);
    void stop_submission();
    // Direct path, for a host that already runs GL on its own thread (another renderer's
    // worker, for example). Call on that thread with the context current: post must queue
    // a task to run there, after earlier posted tasks. Ownership stays on this thread until
    // the recording thread calls adopt_thread(). stop_submission() waits for posted tasks.
    void start_submission_on(std::function<void(std::function<void()>)> post);
    // Owner thread: submits this frame's GX work and retires the previous frame, without
    // presenting. With start_submission_on, the host then calls present_into on its GL thread
    // after the posted work, in task order.
    void end_frame();
    // Host GL thread only: draws the frame's top-left source_width x source_height pixels (the
    // game's display-copy source) into rectangle (x, y, width, height) of the host's
    // framebuffer, lower-left origin, without swapping.
    void present_into(unsigned framebuffer, int source_width, int source_height, int x, int y, int width, int height);
    void set_swap(std::function<void()>);
    void release_snapshots() override;
    void present(int drawable_width, int drawable_height);
    void copy_display(const DisplayCopy&, std::uint8_t*, std::size_t) override;
    void copy_texture(const TextureCopy&, std::uint8_t*, std::size_t) override;
    // With the direct path active, copies stay GPU-resident (gles_direct.hpp),
    // display copies are presented from the GPU without writing XFB memory, and
    // wait_until_idle only submits pending draws: no CPU-visible GX result is
    // pending, and presentation waits for the previous frame.
    void copy_texture_resident(const TextureCopy&, std::uint8_t*, std::size_t, std::uint32_t token) override;
    void release_resident(std::uint32_t token, std::size_t bytes) override;
    // With the direct path active, XFBs change only through display copies, so a
    // retrace showing the same XFB and extent as the last presentation returns false
    // without presenting or swapping; the previous image stays on screen.
    bool present_framebuffer(const FramebufferView&, int drawable_width, int drawable_height, bool black = false);
    void wait_until_idle() override;
    // Swaps the EFB colour for a cleared spare and back, so only the 160x114 and
    // 80x57 levels and one additive full-screen pass are drawn. Fast-shading
    // approximation: copies keep 8-bit channels and the 80x57 level samples the
    // 160x114 one directly instead of its box-filtered half.
    void begin_bloom() override;
    void end_bloom(const EfbBloom&) override;
    // Draws, clears and copies between these target a separate EFB-sized colour and depth
    // buffer, cleared to transparent black and the far plane; end_offscreen returns to the
    // EFB as it was. For ports whose PC code renders post-processing passes off screen
    // instead of in an EFB corner it saves and restores. Not nestable.
    void begin_offscreen();
    void end_offscreen();
    // RGBA8 rows from the lower-left, for bounded development validation.
    std::vector<std::uint8_t> readback();
    GlesStats stats() const;
    // A GL failure made the renderer unusable; draws rejected before submission do not.
    bool failed() const;
    // Direct path: see GlesDirect::program_builder and build_programs_in_background. Owner
    // thread; starts the direct path, and so its program builder, if no draw has yet.
    GlesProgramBuilderStats program_builder();
    void build_programs_in_background();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace starlet::gx
