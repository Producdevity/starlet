// Draw-sync callbacks with GpuSetup::peek_lag: a token whose previous-frame snapshot is not read back
// yet is answered from the one before, without the GPU thread waiting; with the setting off it waits.
// Snapshot references are balanced either way.
#include "starlet/compat/gx_fifo.h"
#include "starlet/gx_fifo/gpu.hpp"
#include "starlet/gx_fifo/renderer.hpp"
#include "starlet/os/game_memory.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/sdk/gx.h"
#include "starlet/sdk/vi.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace starlet;
using namespace starlet::sdk;

namespace {

void require(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "peek_lag: %s\n", what);
        std::fflush(nullptr);
        std::_Exit(1);
    }
}

// The EFB's value is the number of idle points (draw done and tokens) the GPU thread has passed.
// Snapshots hold that value as of the point they were taken; released slots are reused.
class GroupRenderer final : public gx_fifo::Renderer {
public:
    void draw(const gx::State&, const gx::Draw&, gx::MemoryReader&) override {}
    void wait_until_idle() override {
        while (hold.load()) std::this_thread::yield();
        ++idle_;
    }
    void copy_display(const gx::DisplayCopy&, std::uint8_t*, std::size_t) override {}
    void copy_texture(const gx::TextureCopy&, std::uint8_t*, std::size_t, std::uint32_t) override {}
    std::uint32_t peek_depth(int, int) override {
        ++live_peeks;
        return selected_ >= 0 ? values_[selected_] : idle_.load();
    }
    std::uint32_t peek_color(int x, int y) override { return peek_depth(x, y) | 0xff000000u; }
    bool present(const gx::FramebufferView&, int, int, bool) override { return true; }
    std::vector<std::uint8_t> readback() override { return {}; }
    void print_stats(const gx_fifo::GpuCounters&) override {}

    int snapshot_efb(const gx::EfbRegion& region) override { return take(region, false); }
    int snapshot_efb_exact(const gx::EfbRegion& region) override { return take(region, true); }
    bool snapshot_ready(int index) const override { return ready_[index].load(); }
    void wait_snapshot(int index) override {
        ++forced_waits;
        ready_[index] = true;
    }
    bool peek_depth_snapshot(int index, int x, int y, std::uint32_t& value) const override {
        if (!ready_[index] || !regions_[index].contains(x, y)) return false;
        ++snapshot_peeks;
        value = values_[index];
        return true;
    }
    bool peek_color_snapshot(int index, int x, int y, std::uint32_t& value) const override {
        if (!peek_depth_snapshot(index, x, y, value)) return false;
        value |= 0xff000000u;
        return true;
    }
    void select_snapshot(int index) override { selected_ = index; }
    void release_snapshot(int index) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require(held_[index] > 0, "a snapshot is released once per reference");
        --held_[index];
        ++released;
    }
    void retain_snapshot(int index) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require(held_[index] > 0, "only a held snapshot is retained");
        ++held_[index];
    }

    int held() {
        std::lock_guard<std::mutex> lock(mutex_);
        int n = 0;
        for (int h : held_) n += h;
        return n;
    }
    gx::EfbRegion last_region() {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_region_;
    }

    static constexpr int slots = 16;
    std::atomic<bool> ready_at_once{true}, hold{false};
    std::atomic<int> taken{0}, exact{0}, released{0}, live_peeks{0}, forced_waits{0};
    mutable std::atomic<int> snapshot_peeks{0};

private:
    int take(const gx::EfbRegion& region, bool full) {
        std::lock_guard<std::mutex> lock(mutex_);
        int index = 0;
        while (index < slots && held_[index]) ++index;
        require(index < slots, "too many snapshots held");
        ++taken;
        exact += full;
        values_[index] = idle_.load();
        regions_[index] = region;
        last_region_ = region;
        ready_[index] = ready_at_once.load();
        held_[index] = 1;
        return index;
    }
    std::mutex mutex_;
    std::atomic<std::uint32_t> idle_{0};
    std::uint32_t values_[slots] = {};
    gx::EfbRegion regions_[slots], last_region_;
    std::atomic<bool> ready_[slots] = {};
    int held_[slots] = {};
    int selected_ = -1;
};

GroupRenderer renderer;
std::uint8_t* arena = nullptr;
bool lag = false;
std::vector<u32> peeked;

void bp(std::uint8_t address, std::uint32_t value) {
    GX_WRITE_U8(GX_FIFO_CMD_LOAD_BP_REG);
    GX_WRITE_U32(std::uint32_t{address} << 24 | (value & 0xffffff));
}
void token(std::uint16_t value) {
    bp(0x48, value);
    bp(0x47, value);
}
void display_copy() {
    std::uint8_t* xfb = arena + 4 * 64 * 1024;
    bp(0x4e, 256);
    bp(0x49, 0);
    bp(0x4a, (447u << 10) | 639u);
    bp(0x4d, 640 * 2 / 32);
    bp(0x4b, os::native_to_physical(xfb) >> 5);
    bp(0x52, 1u << 14);
}

int guest_main() {
    GXFifoObj fifo{};
    __GXFifoInit();
    GXInitFifoBase(&fifo, arena, 64 * 1024);
    GXSetCPUFifo(&fifo);
    GXSetGPFifo(&fifo);
    __GXPEInit();
    GXSetDrawSyncCallback([](u16 value) {
        if (value != 0x10) return;
        u32 z = 0;
        GXPeekZ(100, 200, &z);
        peeked.push_back(z);
    });
    // Frame 0: the first callback reads the live EFB and teaches the token.
    token(0x10);
    GXDrawDone();
    display_copy();
    // Frame 1: the token's own snapshot (a first appearance waits for it by design).
    token(0x10);
    GXDrawDone();
    const u32 frame1 = peeked.back();
    display_copy();
    // Frame 2: answered from frame 1's snapshot. Its own snapshot is not read back for a while.
    renderer.ready_at_once = false;
    token(1);
    token(0x10);
    GXDrawDone();
    require(peeked.back() == frame1, "the next frame's token reads the previous frame's snapshot");
    display_copy();
    // Frame 3: last frame's snapshot is not ready; frame 1's is.
    const int forced = renderer.forced_waits;
    token(1);
    token(0x10);
    GXDrawDone();
    if (lag) {
        require(peeked.back() == frame1 && renderer.forced_waits == forced, "peek_lag answers from the snapshot two frames back without waiting");
    } else {
        require(peeked.back() != frame1 && renderer.forced_waits > forced, "without peek_lag the GPU thread waits for last frame's snapshot");
    }
    renderer.ready_at_once = true;
    display_copy();
    // Frame 4: everything is ready; answered from the last frame in both modes.
    const u32 before = peeked.size();
    token(1);
    token(0x10);
    GXDrawDone();
    require(peeked.size() == before + 1, "the callback ran");
    // A CPU write into the EFB makes the token read its own snapshot, lag or not.
    display_copy();
    gx_fifo::note_efb_poke();
    token(0x10);
    GXDrawDone();
    GXSetDrawSyncCallback(nullptr);
    for (int i = 0; i < 4; ++i) display_copy();
    VIFlush();
    VIFlush();
    GXDrawDone();
    require(renderer.held() == 0, "every snapshot reference is released");
    require(gx_fifo::gpu_failure()[0] == 0, "no command stream failure");
    return 0;
}

}

int main(int argc, char** argv) {
    lag = argc > 1 && std::strcmp(argv[1], "lag") == 0;
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
    setup.peek_lag = lag;
    gx_fifo::start_gpu(setup);
    const int result = os::run_main_thread(guest_main);
    std::printf("peek_lag%s: %s\n", lag ? " (on)" : " (off)", result == 0 ? "passed" : "failed");
    std::fflush(nullptr);
    std::_Exit(result);
}
