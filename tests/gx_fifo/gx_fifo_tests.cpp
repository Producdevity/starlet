// The GX FIFO's guest side against the GPU thread, with a renderer that records what reaches it:
// staging and chunk submission, unlinked FIFOs, token and draw-done ordering, EFB peeks, display
// copies and VI presentation.
#include "starlet/compat/gx_fifo.h"
#include "starlet/gx_fifo/gpu.hpp"
#include "starlet/gx_fifo/renderer.hpp"
#include "starlet/gx_fifo/video.hpp"
#include "starlet/os/game_memory.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/sdk/gx.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace starlet;
using namespace starlet::sdk;

namespace {

void require(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "gx_fifo: %s\n", what);
        std::fflush(nullptr);
        // Other threads (GPU, retrace) still run: leave without static destructors.
        std::_Exit(1);
    }
}

class RecordingRenderer final : public gx_fifo::Renderer {
public:
    void draw(const gx::State&, const gx::Draw&, gx::MemoryReader&) override { add("draw"); }
    void wait_until_idle() override { add("idle"); }
    void copy_display(const gx::DisplayCopy& copy, std::uint8_t* destination, std::size_t bytes) override {
        std::memset(destination, 0x80, bytes);
        add("display " + std::to_string(copy.width) + "x" + std::to_string(copy.height));
    }
    void copy_texture(const gx::TextureCopy&, std::uint8_t*, std::size_t, std::uint32_t) override { add("texture"); }
    std::uint32_t peek_depth(int x, int y) override { return std::uint32_t(x * 1000 + y); }
    std::uint32_t peek_color(int x, int y) override { return 0xff000000u | std::uint32_t(x << 8 | y); }
    bool present(const gx::FramebufferView& frame, int, int, bool black) override {
        add(black ? std::string("present black") : "present " + std::to_string(frame.width) + "x" + std::to_string(frame.height));
        return true;
    }
    std::vector<std::uint8_t> readback() override { return {}; }
    void print_stats(const gx_fifo::GpuCounters&) override {}

    std::vector<std::string> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto events = std::move(events_);
        events_.clear();
        return events;
    }
    bool saw(const std::string& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& e : events_)
            if (e == event) return true;
        return false;
    }

private:
    void add(std::string event) {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(std::move(event));
    }
    std::mutex mutex_;
    std::vector<std::string> events_;
};

RecordingRenderer renderer;
unsigned dirty_state_flushes = 0;
std::vector<std::string> interrupts;
std::uint8_t* arena = nullptr;

void bp(std::uint8_t address, std::uint32_t value) {
    GX_WRITE_U8(GX_FIFO_CMD_LOAD_BP_REG);
    GX_WRITE_U32(std::uint32_t{address} << 24 | (value & 0xffffff));
}

void token(std::uint16_t value) {
    bp(0x48, value);
    bp(0x47, value);
}

int guest_main() {
    GXFifoObj fifo_a{}, fifo_b{}, fifo_c{};
    constexpr u32 fifo_size = 64 * 1024;
    __GXFifoInit();
    GXInitFifoBase(&fifo_a, arena, fifo_size);
    GXInitFifoBase(&fifo_b, arena + fifo_size, fifo_size);
    GXInitFifoBase(&fifo_c, arena + 2 * fifo_size, fifo_size);
    GXSetCPUFifo(&fifo_a);
    GXSetGPFifo(&fifo_a);
    __GXPEInit();
    GXSetDrawSyncCallback([](u16 value) { interrupts.push_back("token " + std::to_string(value)); });
    GXSetDrawDoneCallback([] { interrupts.push_back("done"); });

    // Linked: staged bytes reach the GPU at GXFlush; the token interrupt precedes draw done, each after
    // the renderer submitted the work before it.
    token(0xbeef);
    GXFlush();
    require(dirty_state_flushes == 1, "GXFlush runs the port's dirty-state flush");
    GXDrawDone();
    require(interrupts == std::vector<std::string>{"token 48879", "done"}, "token then draw done");
    require(GXReadDrawSync() == 0xbeef, "the token register holds the last token");
    require(renderer.take() == std::vector<std::string>{"idle", "idle"}, "the renderer idles before each interrupt");

    // Commands spanning several 64 KiB chunks arrive whole and in order.
    interrupts.clear();
    for (int i = 0; i < 200000; ++i) GX_WRITE_U8(0);
    token(0x1234);
    GXDrawDone();
    require(interrupts == std::vector<std::string>{"token 4660", "done"}, "a token after 200000 NOPs");

    // Unlinked CPU FIFO: writes land in its memory, big-endian, until the GPU is pointed at it.
    interrupts.clear();
    GXSetCPUFifo(&fifo_b);
    token(0x5678);
    const std::uint8_t expected[] = {0x61, 0x48, 0x00, 0x56, 0x78, 0x61, 0x47, 0x00, 0x56, 0x78};
    require(std::memcmp(arena + fifo_size, expected, sizeof expected) == 0, "unlinked writes land in FIFO memory");
    void *read = nullptr, *write = nullptr;
    GXGetFifoPtrs(&fifo_b, &read, &write);
    require(read == arena + fifo_size && write == arena + fifo_size + 10, "the write pointer follows the writes");
    GXBool overhi, underflow, cpu_write, gp_read, wrapped;
    u32 count = 0;
    GXGetFifoStatus(&fifo_b, &overhi, &underflow, &count, &cpu_write, &gp_read, &wrapped);
    require(count == 10 && cpu_write && !gp_read && !wrapped && underflow && !overhi, "unlinked FIFO status");
    require(GXReadDrawSync() == 0x1234, "the GPU has not run the unlinked FIFO");
    GXSetGPFifo(&fifo_b);
    GXDrawDone();
    require(interrupts == std::vector<std::string>{"token 22136", "done"}, "GXSetGPFifo runs what the CPU wrote");

    // An unlinked FIFO wraps at its end, as the console's does.
    GXSetCPUFifo(&fifo_c);
    for (u32 i = 0; i < fifo_size; ++i) GX_WRITE_U8(0);
    GXGetFifoStatus(&fifo_c, &overhi, &underflow, &count, &cpu_write, &gp_read, &wrapped);
    require(wrapped, "the wrap is reported");
    GXSetCPUFifo(&fifo_b);
    GXGetFifoStatus(&fifo_b, &overhi, &underflow, &count, &cpu_write, &gp_read, &wrapped);
    require(!wrapped, "setting the CPU FIFO clears the wrap");

    // Peeks run after every command written so far.
    u32 z = 0, color = 0;
    GXPeekZ(3, 4, &z);
    GXPeekARGB(5, 6, &color);
    require(z == 3004 && color == 0xff000506u, "EFB peeks reach the renderer");

    // A display copy into an XFB, then VI shows it.
    std::uint8_t* xfb = arena + 4 * fifo_size;
    bp(0x4e, 256);
    bp(0x49, 0);
    bp(0x4a, (447u << 10) | 639u);
    bp(0x4d, 640 * 2 / 32);
    bp(0x4b, os::native_to_physical(xfb) >> 5);
    bp(0x52, 1u << 14);
    GXDrawDone();
    require(renderer.saw("display 640x448"), "the display copy reaches the renderer");
    require(xfb[0] == 0x80, "the copy wrote game memory");
    gx_fifo::set_video_presenter(gx_fifo::present);
    VIInit();
    VISetNextFrameBuffer(xfb);
    VISetBlack(FALSE);
    VIFlush();
    VIWaitForRetrace();
    VIWaitForRetrace();
    require(VIGetRetraceCount() >= 2, "retraces count");
    __GXAbort();
    require(renderer.saw("present 640x448"), "VI presents the copied XFB");
    require(gx_fifo::gpu_failure()[0] == 0, "no command stream failure");
    return 0;
}

}

int main() {
    constexpr std::size_t arena_size = 16 * 1024 * 1024;
    // The arena opens the memory window: the GX's copy destinations lie in its first 64 MiB.
    arena = static_cast<std::uint8_t*>(os::map_fixed(0x10000000, arena_size, "test arena"));
    os::set_memory_map({0x10000000});
    os::set_arena(arena, arena_size);
    const std::uintptr_t hints[] = {0x30000000, 0x20000000};
    os::set_stack_pool(8 * 1024 * 1024, 0x40000000, hints, 2);

    gx_fifo::GpuSetup setup;
    setup.renderer = &renderer;
    setup.efb.height = 448;
    setup.flush_dirty_state = [] { ++dirty_state_flushes; };
    gx_fifo::start_gpu(setup);
    const int result = os::run_main_thread(guest_main);
    std::printf("gx_fifo: %s\n", result == 0 ? "passed" : "failed");
    std::fflush(nullptr);
    // The retrace thread runs until exit.
    std::_Exit(result);
}
