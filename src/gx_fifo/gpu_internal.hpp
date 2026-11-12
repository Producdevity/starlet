#pragma once

// Between the guest-side GX SDK (gx_sdk.cpp) and the GPU thread (gpu.cpp).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace starlet::gx_fifo {

// Guest side: queue command bytes, in order, for the GPU thread.
void gpu_submit(std::vector<std::uint8_t>&& bytes);
// Guest side: wait until the GPU thread has run everything queued so far.
void gpu_drain();
// Guest side: true when the GPU thread has run everything queued so far. Does not wait.
bool gpu_idle();
// Guest side: run call on the GPU thread after everything queued so far and wait for it.
void gpu_call(const std::function<void()>& call);
// Guest side: queue call on the GPU thread without waiting.
void gpu_post(std::function<void()> call);

// GPU thread: EFB reads after all prior draws.
std::uint32_t gpu_peek_depth(int x, int y);
std::uint32_t gpu_peek_color(int x, int y);

// The pixel engine's interrupts, raised on the GPU thread as their commands execute (BP 0x45 draw
// done, BP 0x48 token).
struct GpuEvents {
    void (*draw_done)() = nullptr;
    void (*token)(std::uint16_t) = nullptr;
};
void set_gpu_events(GpuEvents events);
// The token register (last BP 0x47 or 0x48 value executed).
std::uint16_t gpu_token();

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

}
