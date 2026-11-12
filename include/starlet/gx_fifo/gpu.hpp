#pragma once

#include "starlet/gx/state.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace starlet::gx_fifo {

class Renderer;
struct VideoFrame;

// The GX as the game sees it. Guest threads write commands through the write-gather pipe
// (starlet/compat/gx_fifo.h); this module hands them to a GPU thread, which owns Starlet's decoder
// and the renderer and runs the commands in order: draws, EFB copies, the draw-done and token
// interrupts, and VI presentation. GX addresses are physical addresses in starlet::os's memory
// window (starlet/os/game_memory.hpp).

// Development switches; all off by default.
struct Diagnostics {
    // Renderer and decoder counters every 300 retraces.
    bool stats = false;
    // The EFB written as <capture_dir>/efb-<retrace>.ppm at these retraces. The renderer's GL work
    // must run on the GPU thread (no submission thread) for the read-back.
    std::vector<unsigned> efb_dumps;
    std::string capture_dir = "/tmp";
    // The state of the first trace_draws draws from retrace trace_from on, on stdout.
    int trace_draws = 0;
    unsigned trace_from = 0;
};

struct GpuSetup {
    Renderer* renderer = nullptr;
    // The game's EFB frame (Starlet's default 640x456; RE4 and Pikmin 2 use 640x448).
    gx::EfbLayout efb;
    // Runs on the GPU thread before decoding starts (adopt the renderer, start its submission
    // thread) and when it stops.
    std::function<void()> start;
    std::function<void()> stop;
    // Drawable size for presentation, read on the GPU thread.
    std::function<void(int&, int&)> drawable_size;
    // Guest side, required: GXFlush calls it first. The decompilation's GX state lives in its own
    // __GXData, so the port passes a function doing what the SDK's GXFlush does with it:
    // if (__GXData->dirtyState) __GXSetDirtyState();
    void (*flush_dirty_state)() = nullptr;
    Diagnostics diagnostics;
};

// Starts the GPU thread. starlet::os's memory map and arena must be set; the GX SDK and VI calls
// need it running. Throws std::invalid_argument for an incomplete setup.
void start_gpu(GpuSetup setup);
void stop_gpu();

// VI presenter (set_video_presenter): queues presentation of the frame behind the commands written
// so far. Guest CPU only.
void present(const VideoFrame& frame);

// Guest CPU: EFB reads after every command written so far (GXPeekZ, GXPeekARGB).
std::uint32_t peek_depth(int x, int y);
std::uint32_t peek_color(int x, int y);

// The first GX command stream failure, or an empty string. The pointer stays valid until the next call.
const char* gpu_failure();

}
