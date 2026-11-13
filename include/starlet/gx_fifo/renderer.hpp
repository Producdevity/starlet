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
    // Optional EFB snapshots (gx::EfbPeekBackend), for draw-sync callbacks that read the EFB:
    // the GPU thread retains the EFB at their token and the callback reads it once read back.
    // snapshot_ready and the snapshot peeks run on any thread, the rest on the GPU thread.
    virtual int snapshot_efb(const gx::EfbRegion&) { return -1; }
    virtual bool snapshot_ready(int) const { return true; }
    virtual void wait_snapshot(int) {}
    virtual bool peek_depth_snapshot(int, int, int, std::uint32_t&) const { return false; }
    virtual bool peek_color_snapshot(int, int, int, std::uint32_t&) const { return false; }
    virtual void select_snapshot(int) {}
    virtual void release_snapshot(int) {}
    // Shows frame (or black) on a drawable of that size; false when nothing new was shown.
    virtual bool present(const gx::FramebufferView& frame, int drawable_width, int drawable_height, bool black) = 0;
    // Development: the EFB as RGBA8 rows from the lower left; empty when the renderer cannot read it.
    virtual std::vector<std::uint8_t> readback() = 0;
    // Development: one line of counters every 300 retraces (Diagnostics::stats).
    virtual void print_stats(const GpuCounters& gpu) = 0;
};

}
