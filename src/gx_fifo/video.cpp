// The video interface: a 59.94 Hz retrace interrupt, the frame buffer the next retrace shows, and the
// retrace callbacks.
#include "starlet/gx_fifo/frame_trace.hpp"
#include "starlet/gx_fifo/video.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/os/host_allocation.hpp"
#include "starlet/os/thread_name.hpp"
#include "starlet/sdk/gx.h"
#include "starlet/sdk/vi.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

using namespace starlet::sdk;

namespace starlet::gx_fifo {
namespace {

using clock = std::chrono::steady_clock;
// NTSC field rate: 60000/1001 Hz.
constexpr auto retrace_period = std::chrono::nanoseconds(std::int64_t{1001} * 1000000000 / 60000);

VideoPresenter presenter = nullptr;
VIRetraceCallback pre_callback = nullptr;
VIRetraceCallback post_callback = nullptr;
u32 retrace_count = 0;
OSThreadQueue retrace_waiters;

struct Registers {
    void* frame_buffer = nullptr;
    bool black = true;
    u16 width = 640, height = 480;
};
Registers shadow, live;
bool flush_pending = false;
std::atomic<bool> started{false};

void retrace() {
    ++retrace_count;
    frame_event('R', retrace_count);
    if (pre_callback) pre_callback(retrace_count);
    if (flush_pending) {
        live = shadow;
        flush_pending = false;
    }
    if (presenter) presenter(VideoFrame{live.frame_buffer, live.black, live.width, live.height, retrace_count});
    if (post_callback) post_callback(retrace_count);
    OSWakeupThread(&retrace_waiters);
}

void run() {
    os::name_thread("vi-retrace");
    auto next = clock::now() + retrace_period;
    for (;;) {
        std::this_thread::sleep_until(next);
        frame_event('r');
        os::raise_interrupt(retrace);
        next += retrace_period;
        // After a long stall (debugger, suspend) resume at the current time instead of delivering a
        // burst of retraces.
        const auto now = clock::now();
        if (now - next > retrace_period * 8) next = now + retrace_period;
    }
}

}

void set_video_presenter(VideoPresenter p) { presenter = p; }

}

using namespace starlet::gx_fifo;

extern "C" {

void VIInit() {
    if (started.exchange(true)) return;
    starlet::os::host_allocation_scope host;
    OSInitThreadQueue(&retrace_waiters);
    std::thread(run).detach();
}

void VIConfigure(const GXRenderModeObj* mode) {
    shadow.width = mode->fbWidth;
    shadow.height = mode->xfbHeight;
}

void VIFlush() {
    frame_event('F');
    const BOOL enabled = OSDisableInterrupts();
    flush_pending = true;
    OSRestoreInterrupts(enabled);
}

void VISetNextFrameBuffer(void* fb) {
    frame_event('S', std::uint32_t(reinterpret_cast<std::uintptr_t>(fb)));
    const BOOL enabled = OSDisableInterrupts();
    shadow.frame_buffer = fb;
    OSRestoreInterrupts(enabled);
}

void* VIGetNextFrameBuffer() { return shadow.frame_buffer; }
void* VIGetCurrentFrameBuffer() { return live.frame_buffer; }

void VISetBlack(BOOL isBlack) {
    const BOOL enabled = OSDisableInterrupts();
    shadow.black = isBlack != FALSE;
    OSRestoreInterrupts(enabled);
}

void VIWaitForRetrace() {
    const BOOL enabled = OSDisableInterrupts();
    const u32 count = retrace_count;
    do OSSleepThread(&retrace_waiters);
    while (count == retrace_count);
    OSRestoreInterrupts(enabled);
}

u32 VIGetRetraceCount() {
    starlet::os::checkpoint();
    return retrace_count;
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback callback) {
    const BOOL enabled = OSDisableInterrupts();
    const VIRetraceCallback previous = pre_callback;
    pre_callback = callback;
    OSRestoreInterrupts(enabled);
    return previous;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback callback) {
    const BOOL enabled = OSDisableInterrupts();
    const VIRetraceCallback previous = post_callback;
    post_callback = callback;
    OSRestoreInterrupts(enabled);
    return previous;
}

// Progressive scan: every retrace is a whole frame on field 1 of an interlaced count.
u32 VIGetNextField() { return 1; }
u32 VIGetCurrentLine() { return 0; }
u32 VIGetTvFormat() { return VI_NTSC; }
u32 VIGetDTVStatus() { return 0; }

}
