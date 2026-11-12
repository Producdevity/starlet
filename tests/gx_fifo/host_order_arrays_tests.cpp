// GpuSetup::arrays_host_order: with it on, an array of host-order numbers and a range registered with
// starlet_gx_big_endian_array holding the same numbers as canonical bytes decode to the same vertices.
#include "starlet/compat/gx_fifo.h"
#include "starlet/gx/vertex.hpp"
#include "starlet/gx_fifo/gpu.hpp"
#include "starlet/gx_fifo/renderer.hpp"
#include "starlet/os/game_memory.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/sdk/gx.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

using namespace starlet;
using namespace starlet::sdk;

namespace {

void require(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "host_order_arrays: %s\n", what);
        std::fflush(nullptr);
        std::_Exit(1);
    }
}

using Positions = std::vector<std::array<float, 3>>;

class CapturingRenderer final : public gx_fifo::Renderer {
public:
    void draw(const gx::State& state, const gx::Draw& draw, gx::MemoryReader& memory) override {
        const auto result = gx::capture_vertices(state, draw, memory);
        Positions positions;
        if (result)
            for (const auto& v : result.snapshot->vertices()) positions.push_back(v.position);
        std::lock_guard<std::mutex> lock(mutex_);
        draws_.push_back(positions);
    }
    void wait_until_idle() override {}
    void copy_display(const gx::DisplayCopy&, std::uint8_t*, std::size_t) override {}
    void copy_texture(const gx::TextureCopy&, std::uint8_t*, std::size_t, std::uint32_t) override {}
    std::uint32_t peek_depth(int, int) override { return 0; }
    std::uint32_t peek_color(int, int) override { return 0; }
    bool present(const gx::FramebufferView&, int, int, bool) override { return true; }
    std::vector<std::uint8_t> readback() override { return {}; }
    void print_stats(const gx_fifo::GpuCounters&) override {}

    std::vector<Positions> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto draws = std::move(draws_);
        draws_.clear();
        return draws;
    }

private:
    std::mutex mutex_;
    std::vector<Positions> draws_;
};

CapturingRenderer renderer;
std::uint8_t* arena = nullptr;

void cp(std::uint8_t address, std::uint32_t value) {
    GX_WRITE_U8(0x08);  // CP register load
    GX_WRITE_U8(address);
    GX_WRITE_U32(value);
}

// One triangle of positions 0, 1 and 2 from the position array at base: XYZ s16, 8-bit indices.
void draw_triangle(const void* base) {
    cp(0x50, 2u << 9);
    cp(0x70, 1u | (3u << 1));
    cp(0xa0, os::native_to_physical(base));
    cp(0xb0, 6);
    GX_WRITE_U8(0x90);
    GX_WRITE_U16(3);
    for (std::uint8_t i = 0; i < 3; ++i) GX_WRITE_U8(i);
}

int guest_main() {
    GXFifoObj fifo{};
    constexpr u32 fifo_size = 64 * 1024;
    __GXFifoInit();
    GXInitFifoBase(&fifo, arena, fifo_size);
    GXSetCPUFifo(&fifo);
    GXSetGPFifo(&fifo);
    __GXPEInit();

    const std::int16_t values[9] = {1, -2, 300, -4000, 5, 6, 7, -8, 32767};
    // Host order, as the port's converters leave loaded vertex data.
    auto* host = reinterpret_cast<std::int16_t*>(arena + fifo_size);
    std::memcpy(host, values, sizeof values);
    // The same numbers as canonical big-endian bytes, registered as such.
    std::uint8_t* canonical = arena + fifo_size + 64;
    for (int i = 0; i < 9; ++i) {
        canonical[2 * i] = std::uint8_t(std::uint16_t(values[i]) >> 8);
        canonical[2 * i + 1] = std::uint8_t(values[i]);
    }
    starlet_gx_big_endian_array(canonical, sizeof values);

    draw_triangle(host);
    draw_triangle(canonical);
    GXDrawDone();
    const auto draws = renderer.take();
    require(draws.size() == 2, "both draws reach the renderer");
    const Positions expected = {{1, -2, 300}, {-4000, 5, 6}, {7, -8, 32767}};
    require(draws[0] == expected, "the host-order array decodes to its numbers");
    require(draws[1] == expected, "the registered big-endian range decodes to the same numbers");

    // Released, the canonical bytes read as host order like every other array.
    starlet_gx_release_array(canonical);
    draw_triangle(canonical);
    GXDrawDone();
    const auto released = renderer.take();
    require(released.size() == 1 && released[0] != expected, "a released range reads as host order");
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

    gx_fifo::GpuSetup setup;
    setup.renderer = &renderer;
    setup.efb.height = 448;
    setup.flush_dirty_state = [] {};
    setup.arrays_host_order = true;
    gx_fifo::start_gpu(setup);
    const int result = os::run_main_thread(guest_main);
    std::printf("host_order_arrays: %s\n", result == 0 ? "passed" : "failed");
    std::fflush(nullptr);
    std::_Exit(result);
}
