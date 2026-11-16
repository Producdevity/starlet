#pragma once

// Between the guest-side GX SDK (gx_sdk.cpp) and the GPU thread (gpu.cpp).

#include "starlet/os/host_allocation.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace starlet::gx_fifo {

// Guest side: queue the first `used` command bytes, in order, for the GPU thread. Once they have
// run, the buffer is kept for gpu_take_buffer.
void gpu_submit(std::vector<std::uint8_t>&& bytes, std::size_t used);
// Guest side: a buffer of `size` bytes, one the GPU thread has finished with when there is one.
// Its bytes are not cleared.
std::vector<std::uint8_t> gpu_take_buffer(std::size_t size);
// Guest side: wait until the GPU thread has run everything queued so far.
void gpu_drain();
// Guest side: true when the GPU thread has run everything queued so far. Does not wait.
bool gpu_idle();
// Guest side: run call on the GPU thread after everything queued so far and wait for it.
void gpu_call_function(const std::function<void()>& call);
// Guest side: queue call on the GPU thread without waiting.
void gpu_post_function(std::function<void()> call);
// The std::function is made outside the game's heaps: a guest thread may call while its
// current heap is gone (Resident Evil 4 transfers its heap back from ARAM before reinstating it).
template<class F> void gpu_call(F&& call) {
    os::host_allocation_scope host;
    gpu_call_function(std::function<void()>(std::forward<F>(call)));
}
template<class F> void gpu_post(F&& call) {
    os::host_allocation_scope host;
    gpu_post_function(std::function<void()>(std::forward<F>(call)));
}

// GPU thread: EFB reads after all prior draws.
std::uint32_t gpu_peek_depth(int x, int y);
std::uint32_t gpu_peek_color(int x, int y);

// The pixel engine's interrupts, raised on the GPU thread in command order (BP 0x45 draw done, BP
// 0x48 token). A token whose callback read the EFB before carries an EFB snapshot taken as it
// executed, and it and later interrupts are raised once that snapshot is read back; the guest
// releases it with gpu_release_snapshot. Other tokens carry -1.
struct GpuEvents {
    void (*draw_done)() = nullptr;
    void (*token)(std::uint16_t token, int snapshot) = nullptr;
};
void set_gpu_events(GpuEvents events);
// The token register (last BP 0x47 or 0x48 value executed).
std::uint16_t gpu_token();

// Guest side, in a draw-sync callback: it read EFB pixels within (x0, y0)-(x1, y1). Later tokens of
// that value snapshot the EFB.
void gpu_note_token_peek(std::uint16_t token, int x0, int y0, int x1, int y1);
// Any thread: a snapshot's pixel once read back and inside its read-back region, else false.
bool gpu_peek_snapshot(int index, bool depth, int x, int y, std::uint32_t& value);
// GPU thread: a snapshot's pixel, waiting for its readback (and queued GL work outside its region).
std::uint32_t gpu_peek_snapshot_wait(int index, bool depth, int x, int y);
// GPU thread: the snapshot may be reused.
void gpu_release_snapshot(int index);
// GPU thread: request, wait for and release an asynchronous EFB depth readback; any thread: a
// pixel of one that has arrived (see Renderer::request_depth_readback).
int gpu_request_depth_readback();
bool gpu_depth_readback_value(int handle, int x, int y, std::uint32_t& value);
void gpu_wait_depth_readback(int handle);
void gpu_release_depth_readback(int handle);

// GpuSetup::sync_reads, or STARLET_GX_SYNC_READS=1.
bool gpu_sync_reads();
// Guest peek groups (gx_sdk.cpp peek): two frames of up to four groups, in slots. GPU thread, in
// command order: the slot's snapshot becomes an exact one of the EFB region (x, y, width, height;
// empty: all of it) as of now, for that guest frame, and the one it held is released;
// gpu_release_group_snapshot only releases it. Any thread: the slot's snapshot if it was taken
// for frame, else -1.
constexpr int peek_group_slots = 8;
void gpu_take_group_snapshot(int slot, std::uint32_t frame, int x, int y, int width, int height);
void gpu_release_group_snapshot(int slot);
int gpu_group_snapshot(int slot, std::uint32_t frame);
// GPU thread, in command order: the CPU wrote EFB pixels (note_efb_poke).
void gpu_note_poke();
// Any thread: a peek group was answered from a snapshot one frame earlier (for the stats line).
void gpu_count_late_read();

// GPU thread: native-order arrays (see starlet/compat/gx_fifo.h), in command order.
void gpu_host_order_array(std::uintptr_t base, std::size_t bytes);
void gpu_big_endian_array(std::uintptr_t base, std::size_t bytes);
void gpu_release_array(std::uintptr_t base);

// Guest side: the VI shows frame_buffer (or black) at this retrace.
void gpu_present(const void* frame_buffer, bool black);

// Guest side: the port's GXFlush prologue (GpuSetup::flush_dirty_state).
void flush_dirty_state();

// Guest side: hands the staged bytes of a linked FIFO to the GPU thread.
void flush_stage();

// Guest side, at VIFlush: a frame of peek groups ends (gx_sdk.cpp).
void end_peek_frame();

}
