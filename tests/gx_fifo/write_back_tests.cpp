// Texture copies into a written-back range (Renderer::write_back), on the GLES renderer over a
// recording GL fake: no interrupt waits for a copy's readback, the bytes reach memory after it
// arrives, and a later copy into the same memory lands the earlier one first.
#include "starlet/compat/gx_fifo.h"
#include "starlet/gx/texture_copy.hpp"
#include "starlet/gx_fifo/gles_renderer.hpp"
#include "starlet/gx_fifo/gpu.hpp"
#include "starlet/os/game_memory.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/os/os_core.hpp"
#include "starlet/sdk/gx.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace fake {
extern bool map_pattern;
extern int read_pattern_row;
extern std::atomic<bool> hold_readbacks, readback_held;
}

using namespace starlet;
using namespace starlet::sdk;

namespace {

void require(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "gx_fifo write-back: %s\n", what);
        std::fflush(nullptr);
        std::_Exit(1);
    }
}

constexpr std::size_t copy_bytes = 512;
std::uint8_t* arena = nullptr;
std::uint8_t* destination = nullptr;
gx::GlesRenderer* renderer = nullptr;
std::vector<std::string> interrupts;

void bp(std::uint8_t address, std::uint32_t value) {
    GX_WRITE_U8(GX_FIFO_CMD_LOAD_BP_REG);
    GX_WRITE_U32(std::uint32_t{address} << 24 | (value & 0xffffff));
}

void token(std::uint16_t value) {
    bp(0x48, value);
    bp(0x47, value);
}

// A 16x8 RGBA8 copy of the EFB's top-left corner to destination.
void copy() {
    bp(0x49, 0);
    bp(0x4a, (7u << 10) | 15u);
    bp(0x4d, 256 / 32);
    bp(0x4b, os::native_to_physical(destination) >> 5);
    bp(0x52, (2u << 15) | (6u << 4));
}

// The copy's bytes: the fake's readback pattern, stored as the CPU encoder does.
std::array<std::uint8_t, copy_bytes> stored() {
    gx::TextureCopy p;
    p.width = 16; p.height = 8; p.format = 6; p.stride = 256;
    std::vector<std::uint8_t> pixels(16 * 8 * 4);
    for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = std::uint8_t(i % 251);
    alignas(32) std::array<std::uint8_t, copy_bytes> out{};
    gx::store_texture_copy(p, {pixels.data(), pixels.size()}, out.data(), out.size());
    return out;
}

std::array<std::uint8_t, copy_bytes> expected;

bool held() {
    const auto start = std::chrono::steady_clock::now();
    while (!fake::readback_held && std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) std::this_thread::yield();
    return fake::readback_held;
}

// Until `count` copies have landed. Two draw dones a round: the GPU thread finishes the first
// one's command, landing what was read back by then, before it starts the second.
bool lands(std::uint64_t count) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        GXDrawDone();
        GXDrawDone();
        if (renderer->stats().write_backs == count) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

int guest_main() {
    GXFifoObj fifo{};
    __GXFifoInit();
    GXInitFifoBase(&fifo, arena, 64 * 1024);
    GXSetCPUFifo(&fifo);
    GXSetGPFifo(&fifo);
    __GXPEInit();
    expected = stored();
    GXSetDrawSyncCallback([](u16 value) {
        const bool copied = std::equal(expected.begin(), expected.end(), destination);
        interrupts.push_back("token " + std::to_string(value) + (copied ? " copied" : " stale"));
    });
    GXSetDrawDoneCallback([] { interrupts.push_back("done"); });

    // The readback held on the GL thread: the token and the draw done behind the copy are raised
    // meanwhile, with memory as it was.
    std::memset(destination, 0x11, copy_bytes);
    fake::hold_readbacks = true;
    copy();
    token(0x20);
    GXFlush();
    require(held(), "the copy is read back on the GL thread");
    GXDrawDone();
    require(interrupts == std::vector<std::string>{"token 32 stale", "done"} && destination[0] == 0x11,
            "the token and the draw done do not wait for the copy's readback");
    fake::hold_readbacks = false;
    require(lands(1), "the copy's bytes reach memory once read back");
    require(std::equal(expected.begin(), expected.end(), destination), "memory holds the copy");
    interrupts.clear();

    // A second copy into the same memory while the first one's readback is held: the first lands
    // before the second is made, and nothing is dropped.
    std::memset(destination, 0x22, copy_bytes);
    fake::hold_readbacks = true;
    copy();
    GXFlush();
    require(held(), "the first copy is read back on the GL thread");
    copy();
    token(0x21);
    GXFlush();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    require(interrupts.empty() && destination[0] == 0x22, "the second copy waits for the first one's bytes");
    fake::hold_readbacks = false;
    require(lands(3), "both copies land");
    require(interrupts.front() == "token 33 copied", "the token behind the second copy sees the first one's bytes");
    require(renderer->stats().write_backs_dropped == 0, "no copy is dropped");
    require(gx_fifo::gpu_failure()[0] == 0, "no command stream failure");
    return 0;
}

}

int main() {
    constexpr std::size_t arena_size = 16 * 1024 * 1024;
    arena = static_cast<std::uint8_t*>(os::map_fixed(0x10000000, arena_size, "test arena"));
    os::set_memory_map({0x10000000});
    os::set_arena(arena, arena_size);
    const std::uintptr_t hints[] = {0x30000000, 0x20000000};
    os::set_stack_pool(8 * 1024 * 1024, 0x40000000, hints, 2);
    os::initialize_os({});
    destination = arena + (1u << 20);

    fake::map_pattern = true;
    fake::read_pattern_row = 16;
    gx::GlesLimits limits;
    limits.efb.height = 448;
    static gx::GlesRenderer gles(limits);
    renderer = &gles;
    static gx_fifo::GlesBackend backend(gles);
    gx_fifo::GpuSetup setup;
    setup.renderer = &backend;
    setup.efb = limits.efb;
    setup.flush_dirty_state = [] {};
    // GL work runs on the renderer's own submission thread, as in the ports.
    setup.start = [] {
        renderer->adopt_thread();
        renderer->start_submission([] {}, [] {}, [] {});
        renderer->program_builder();
        backend.write_back(os::native_to_physical(destination), copy_bytes, true);
    };
    gx_fifo::start_gpu(setup);
    const int result = os::run_main_thread(guest_main);
    std::printf("gx_fifo write-back: %s\n", result == 0 ? "passed" : "failed");
    std::fflush(nullptr);
    std::_Exit(result);
}
