// Draw-sync callbacks that read the EFB: the first reads the live EFB; later tokens of that value
// carry a snapshot taken as the token executed, with the pixels read so far as its region, and the
// callback reads it instead of waiting for the work queued after the token. Interrupts stay in
// command order, a token waiting for its readback holds back the draw done behind it, and every
// snapshot is released.
#include "starlet/compat/gx_fifo.h"
#include "starlet/gx_fifo/gpu.hpp"
#include "starlet/gx_fifo/renderer.hpp"
#include "starlet/os/game_memory.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/sdk/gx.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

using namespace starlet;
using namespace starlet::sdk;

namespace {

void require(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "draw_sync_snapshot: %s\n", what);
        std::fflush(nullptr);
        std::_Exit(1);
    }
}

// The EFB's value is the number of idle points (draw done and tokens) the GPU thread has passed.
class SnapshotRenderer final : public gx_fifo::Renderer {
public:
    void draw(const gx::State&, const gx::Draw&, gx::MemoryReader&) override {}
    void wait_until_idle() override { ++idle_; }
    void copy_display(const gx::DisplayCopy&, std::uint8_t*, std::size_t) override {}
    void copy_texture(const gx::TextureCopy&, std::uint8_t*, std::size_t, std::uint32_t) override {}
    std::uint32_t peek_depth(int, int) override {
        ++live_peeks;
        return selected_ >= 0 ? values_[selected_] : idle_.load();
    }
    std::uint32_t peek_color(int x, int y) override { return peek_depth(x, y); }
    bool present(const gx::FramebufferView&, int, int, bool) override { return true; }
    std::vector<std::uint8_t> readback() override { return {}; }
    void print_stats(const gx_fifo::GpuCounters&) override {}

    int snapshot_efb(const gx::EfbRegion& region) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = taken++;
        require(index < slots, "too many snapshots");
        values_[index] = idle_.load();
        regions_[index] = region;
        ready_[index] = ready_at_once.load();
        held_[index] = true;
        return index;
    }
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
        return peek_depth_snapshot(index, x, y, value);
    }
    void select_snapshot(int index) override { selected_ = index; }
    void release_snapshot(int index) override {
        require(held_[index], "a snapshot is released once");
        held_[index] = false;
        ++released;
    }

    gx::EfbRegion region(int index) {
        std::lock_guard<std::mutex> lock(mutex_);
        return regions_[index];
    }

    static constexpr int slots = 16;
    std::atomic<bool> ready_at_once{true};
    std::atomic<int> taken{0}, released{0}, live_peeks{0}, forced_waits{0};
    mutable std::atomic<int> snapshot_peeks{0};

private:
    std::mutex mutex_;
    std::atomic<std::uint32_t> idle_{0};
    std::uint32_t values_[slots] = {};
    gx::EfbRegion regions_[slots];
    std::atomic<bool> ready_[slots] = {};
    bool held_[slots] = {};
    int selected_ = -1;
};

SnapshotRenderer renderer;
std::uint8_t* arena = nullptr;
std::vector<std::string> interrupts;
std::vector<u32> peeked;

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
    constexpr u32 fifo_size = 64 * 1024;
    __GXFifoInit();
    GXInitFifoBase(&fifo, arena, fifo_size);
    GXSetCPUFifo(&fifo);
    GXSetGPFifo(&fifo);
    __GXPEInit();
    GXSetDrawSyncCallback([](u16 value) {
        interrupts.push_back("token " + std::to_string(value));
        if (value != 0x10) return;
        u32 z = 0;
        GXPeekZ(100, 200, &z);
        peeked.push_back(z);
        GXPeekZ(110, 190, &z);
        peeked.push_back(z);
    });
    GXSetDrawDoneCallback([] { interrupts.push_back("done"); });

    // First token 0x10: no snapshot yet, the callback reads the live EFB.
    token(0x10);
    GXDrawDone();
    require(renderer.taken == 0 && renderer.live_peeks == 2, "the first callback reads the live EFB");
    interrupts.clear();
    peeked.clear();

    // Later: a snapshot at the token, over the pixels read last time and a margin; the callback
    // reads the EFB as of the token, though more work follows it.
    token(0x10);
    token(0x11);
    token(0x11);
    GXDrawDone();
    require(renderer.taken == 1, "the token of a reading callback takes a snapshot");
    const auto r = renderer.region(0);
    require(r.contains(100, 200) && r.contains(110, 190) && !r.contains(100 + 64, 200) && !r.empty(),
            "the snapshot reads back the pixels read last time with a margin");
    require(peeked.size() == 2 && peeked[0] == peeked[1] && renderer.snapshot_peeks == 2 && renderer.live_peeks == 2,
            "the callback reads the snapshot, not the live EFB");
    require(interrupts == std::vector<std::string>{"token 16", "token 17", "token 17", "done"}, "interrupts in command order");
    interrupts.clear();
    peeked.clear();

    // A readback not ready yet: the token and the draw done behind it wait; with nothing else
    // queued the GPU thread waits for the readback.
    renderer.ready_at_once = false;
    token(0x10);
    GXDrawDone();
    require(renderer.forced_waits >= 1, "the idle GPU thread waits for the readback");
    require(interrupts == std::vector<std::string>{"token 16", "done"}, "the draw done stays behind the token");
    require(renderer.snapshot_peeks == 4, "the delayed callback reads its snapshot");

    peeked.clear();

    // A pixel outside the read-back region reads the retained snapshot after the queued work.
    GXSetDrawSyncCallback([](u16 value) {
        if (value != 0x10) return;
        u32 z = 0;
        GXPeekZ(600, 20, &z);
        peeked.push_back(z);
    });
    renderer.ready_at_once = true;
    token(0x10);
    token(0x11);
    GXDrawDone();
    require(peeked.size() == 1 && renderer.live_peeks == 3, "a region miss reads through the GPU thread");

    GXSetDrawSyncCallback(nullptr);
    GXDrawDone();
    require(renderer.released == renderer.taken, "every snapshot is released");
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
    gx_fifo::start_gpu(setup);
    const int result = os::run_main_thread(guest_main);
    std::printf("draw_sync_snapshot: %s\n", result == 0 ? "passed" : "failed");
    std::fflush(nullptr);
    std::_Exit(result);
}
