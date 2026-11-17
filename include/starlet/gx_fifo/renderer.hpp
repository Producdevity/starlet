#pragma once

#include "starlet/gx/decoder.hpp"
#include "starlet/gx/display_copy.hpp"
#include "starlet/gx/efb_peek.hpp"
#include "starlet/gx/texture_copy.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace starlet::gx_fifo {

// The GPU thread's counters, for Renderer::print_stats.
struct GpuCounters {
    unsigned retraces = 0;
    bool presented = false;        // the last retrace presented a new image
    const void* frame_buffer = nullptr;  // the XFB it showed
    bool known_frame_buffer = false;     // a display copy wrote that XFB
    std::uint64_t decoded_bytes = 0;
    unsigned skipped_draws = 0;    // draws the renderer refused
    std::uint64_t late_reads = 0;  // peek groups and draw-sync callbacks answered one frame late (GpuSetup::sync_reads)
};

// What the GPU thread hands decoded commands to. Every call comes from the GPU thread, in command
// order. gles_renderer.hpp adapts Starlet's OpenGL ES renderer.
class Renderer {
public:
    virtual ~Renderer() = default;
    // Throws to refuse a draw: the GPU thread reports it once per message and skips it.
    virtual void draw(const gx::State& state, const gx::Draw& draw, gx::MemoryReader& memory) = 0;
    // Before the draw-done and token interrupts: work queued so far is submitted.
    virtual void wait_until_idle() = 0;
    // destination is game memory holding bytes bytes at physical address address.
    virtual void copy_display(const gx::DisplayCopy& copy, std::uint8_t* destination, std::size_t bytes) = 0;
    virtual void copy_texture(const gx::TextureCopy& copy, std::uint8_t* destination, std::size_t bytes,
                              std::uint32_t address) = 0;
    // EFB reads after all prior draws: top-left origin, D24 depth, colour as 0xAARRGGBB.
    virtual std::uint32_t peek_depth(int x, int y) = 0;
    virtual std::uint32_t peek_color(int x, int y) = 0;
    // Host transfers on the game's behalf (starlet::os::before_dma_read, before_dma_write). Any
    // thread: whether results the renderer keeps off memory (resident texture copies) may hold
    // bytes of [data, data + bytes). Only then does the GPU thread call before_dma_read or
    // before_dma_write, after the commands written before the transfer, while the transfer waits:
    // memory about to be read gets those results, memory about to be written drops them.
    virtual bool resident_in(const void*, std::size_t) const { return false; }
    virtual void before_dma_read(const void*, std::size_t) {}
    virtual void before_dma_write(const void*, std::size_t) {}
    // Texture copies into [address, address + bytes) also reach memory, as on the console, for a
    // game whose CPU reads them: each is read back without waiting, and its bytes are written in
    // issue order once read back, at the latest before the next copy into the same memory. No
    // interrupt waits for them. on = false drops the range. write_backs_issued counts such copies
    // so far; land_write_backs writes those read back, up to that count, and is true once all of
    // them are written (wait: blocks until then).
    virtual void write_back(std::uint32_t, std::size_t, bool) {}
    virtual std::uint64_t write_backs_issued() const { return 0; }
    virtual bool land_write_backs(std::uint64_t, bool) { return true; }
    // A texture copy into a write_back range whose bytes the caller computed (contents, bytes long,
    // as the copy encodes them): the renderer keeps its own copy as copy_texture does, and memory
    // takes contents at once instead of a readback. Earlier copies into the range land first.
    virtual void copy_texture_with_contents(const gx::TextureCopy& copy, std::uint8_t* destination, std::size_t bytes,
                                            std::uint32_t address, const std::uint8_t* contents) {
        static_cast<void>(contents);
        copy_texture(copy, destination, bytes, address);
    }
    // Optional EFB snapshots (gx::EfbPeekBackend), for draw-sync callbacks that read the EFB:
    // the GPU thread retains the EFB at their token and the callback reads it once read back.
    // snapshot_ready and the snapshot peeks run on any thread, the rest on the GPU thread.
    virtual int snapshot_efb(const gx::EfbRegion&) { return -1; }
    // As snapshot_efb, at full resolution: peeks read exactly what peek_depth and peek_color
    // would have read at that point (EFB peeks outside draw-sync callbacks).
    virtual int snapshot_efb_exact(const gx::EfbRegion& region) { return snapshot_efb(region); }
    virtual bool snapshot_ready(int) const { return true; }
    virtual void wait_snapshot(int) {}
    virtual bool peek_depth_snapshot(int, int, int, std::uint32_t&) const { return false; }
    virtual bool peek_color_snapshot(int, int, int, std::uint32_t&) const { return false; }
    virtual void select_snapshot(int) {}
    virtual void release_snapshot(int) {}
    virtual void retain_snapshot(int) {}
    // The EFB's depth as of this point, read back without waiting for the work before it
    // (gx::GlesRenderer::request_depth_readback); -1 when the backend reads it live only.
    virtual int request_depth_readback() { return -1; }
    virtual bool depth_readback_value(int, int, int, std::uint32_t&) const { return false; }
    virtual void wait_depth_readback(int) {}
    virtual void release_depth_readback(int) {}
    // GpuSetup::frame_ahead, on the GPU thread before decoding starts: the renderer may queue
    // a second frame for the GPU before the first completes.
    virtual void set_frame_ahead(bool) {}
    // Shows frame (or black) on a drawable of that size; false when nothing new was shown.
    virtual bool present(const gx::FramebufferView& frame, int drawable_width, int drawable_height, bool black) = 0;
    // Development: the EFB as RGBA8 rows from the lower left; empty when the renderer cannot read it.
    virtual std::vector<std::uint8_t> readback() = 0;
    // Development: one line of counters every 300 retraces (Diagnostics::stats).
    virtual void print_stats(const GpuCounters& gpu) = 0;
};

}
