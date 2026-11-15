// GpuSetup::frame_ahead, or STARLET_GX_FRAME_AHEAD=1, reaches the renderer on the GPU thread before
// it decodes a command; draw done, tokens and exact peeks still come after every command written
// before them. Modes: "setup", "env" (run under STARLET_GX_FRAME_AHEAD=1) and "off".
#include "starlet/compat/gx_fifo.h"
#include "starlet/gx_fifo/gpu.hpp"
#include "starlet/gx_fifo/renderer.hpp"
#include "starlet/os/game_memory.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/sdk/gx.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace starlet;
using namespace starlet::sdk;

namespace {

void require(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "frame_ahead: %s\n", what);
        std::fflush(nullptr);
        std::_Exit(1);
    }
}

// The EFB's value is the number of idle points (draw done and tokens) the GPU thread has passed.
class AheadRenderer final : public gx_fifo::Renderer {
public:
    void draw(const gx::State&, const gx::Draw&, gx::MemoryReader&) override {}
    void wait_until_idle() override { ++idle; }
    void copy_display(const gx::DisplayCopy&, std::uint8_t*, std::size_t) override {}
    void copy_texture(const gx::TextureCopy&, std::uint8_t*, std::size_t, std::uint32_t) override {}
    std::uint32_t peek_depth(int, int) override { return idle.load(); }
    std::uint32_t peek_color(int, int) override { return idle.load(); }
    void set_frame_ahead(bool on) override {
        ++calls;
        ahead = on;
        before_commands = idle.load() == 0;
        on_gpu_thread = std::this_thread::get_id() != guest_thread;
    }
    bool present(const gx::FramebufferView&, int, int, bool) override { return true; }
    std::vector<std::uint8_t> readback() override { return {}; }
    void print_stats(const gx_fifo::GpuCounters&) override {}

    std::atomic<std::uint32_t> idle{0};
    std::atomic<int> calls{0};
    std::atomic<bool> ahead{false}, before_commands{false}, on_gpu_thread{false};
    std::thread::id guest_thread;
};

AheadRenderer renderer;
std::uint8_t* arena = nullptr;
std::string mode;
std::vector<std::string> interrupts;

void bp(std::uint8_t address, std::uint32_t value) {
    GX_WRITE_U8(GX_FIFO_CMD_LOAD_BP_REG);
    GX_WRITE_U32(std::uint32_t{address} << 24 | (value & 0xffffff));
}

void token(std::uint16_t value) {
    bp(0x48, value);
    bp(0x47, value);
}

int guest_main() {
    GXFifoObj fifo{};
    __GXFifoInit();
    GXInitFifoBase(&fifo, arena, 64 * 1024);
    GXSetCPUFifo(&fifo);
    GXSetGPFifo(&fifo);
    __GXPEInit();
    GXSetDrawSyncCallback([](u16 value) { interrupts.push_back("token " + std::to_string(value)); });
    GXSetDrawDoneCallback([] { interrupts.push_back("done"); });

    for (int frame = 0; frame < 3; ++frame) {
        interrupts.clear();
        const std::uint32_t before = renderer.idle;
        token(1);
        token(2);
        GXDrawDone();
        require(interrupts == std::vector<std::string>{"token 1", "token 2", "done"}, "interrupts follow every command before them");
        require(renderer.idle == before + 3, "the draw done waits for the GPU thread to run every command");
        token(3);
        gx_fifo::note_efb_poke();
        u32 z = 0;
        GXPeekZ(1, 1, &z);
        require(z == before + 4, "an exact peek reads after every command written before it");
        VIFlush();
    }
    if (mode == "off") require(renderer.calls == 0 && !renderer.ahead, "frame_ahead is off by default");
    else require(renderer.calls == 1 && renderer.ahead && renderer.before_commands && renderer.on_gpu_thread,
                 "frame_ahead reaches the renderer once, on the GPU thread, before any command");
    require(gx_fifo::gpu_failure()[0] == 0, "no command stream failure");
    return 0;
}

}

int main(int argc, char** argv) {
    mode = argc > 1 ? argv[1] : "";
    require(mode == "setup" || mode == "env" || mode == "off", "mode: setup, env or off");
    const char* ahead = std::getenv("STARLET_GX_FRAME_AHEAD");
    require((mode == "env") == (ahead && std::strcmp(ahead, "1") == 0), "env mode runs under STARLET_GX_FRAME_AHEAD=1, the others without");
    constexpr std::size_t arena_size = 16 * 1024 * 1024;
    arena = static_cast<std::uint8_t*>(os::map_fixed(0x10000000, arena_size, "test arena"));
    os::set_memory_map({0x10000000});
    os::set_arena(arena, arena_size);
    const std::uintptr_t hints[] = {0x30000000, 0x20000000};
    os::set_stack_pool(8 * 1024 * 1024, 0x40000000, hints, 2);

    renderer.guest_thread = std::this_thread::get_id();
    gx_fifo::GpuSetup setup;
    setup.renderer = &renderer;
    setup.efb.height = 448;
    setup.flush_dirty_state = [] {};
    setup.frame_ahead = mode == "setup";
    gx_fifo::start_gpu(setup);
    const int result = os::run_main_thread(guest_main);
    std::printf("frame_ahead (%s): %s\n", mode.c_str(), result == 0 ? "passed" : "failed");
    std::fflush(nullptr);
    std::_Exit(result);
}
