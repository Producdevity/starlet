// ARAM and disc DMA over an EFB texture copy that Starlet's GLES renderer (on a recording GL fake)
// keeps on the GPU: the transfers see memory as the console has it once the copy has run. A read
// gets the copy's bytes, a write replaces them, and a later read of a partly written copy gets the
// copy's remaining bytes beside the written ones.
#include "starlet/ar/ar.hpp"
#include "starlet/compat/gx_fifo.h"
#include "starlet/dvd/dvd.hpp"
#include "starlet/gx/texture_copy.hpp"
#include "starlet/gx_fifo/gles_renderer.hpp"
#include "starlet/gx_fifo/gpu.hpp"
#include "starlet/os/game_memory.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/os/os_core.hpp"
#include "starlet/sdk/ar.h"
#include "starlet/sdk/dvd.h"
#include "starlet/sdk/gx.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <ftw.h>
#include <sys/stat.h>

namespace fake {
extern bool map_pattern;
extern int read_pattern_row;
}

extern "C" {
starlet::sdk::u32 ARInit(starlet::sdk::u32* stack_index_addr, starlet::sdk::u32 num_entries);
void ARQPostRequest(starlet::sdk::ARQRequest* task, starlet::sdk::u32 owner, starlet::sdk::u32 type, starlet::sdk::u32 priority,
                    starlet::sdk::u32 source, starlet::sdk::u32 dest, starlet::sdk::u32 length, starlet::sdk::ARQCallback callback);
}

using namespace starlet;
using namespace starlet::sdk;

namespace {

void require(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "gx_fifo dma: %s\n", what);
        std::fflush(nullptr);
        // Other threads (GPU, GL, drive) still run: leave without static destructors.
        std::_Exit(1);
    }
}

constexpr std::size_t copy_bytes = 512;
std::uint8_t* arena = nullptr;
gx::GlesRenderer* renderer = nullptr;
std::string disc_root;

void bp(std::uint8_t address, std::uint32_t value) {
    GX_WRITE_U8(GX_FIFO_CMD_LOAD_BP_REG);
    GX_WRITE_U32(std::uint32_t{address} << 24 | (value & 0xffffff));
}

// A 16x8 RGBA8 copy of the EFB's top-left corner to destination, run before returning.
void copy_to(std::uint8_t* destination) {
    bp(0x49, 0);
    bp(0x4a, (7u << 10) | 15u);
    bp(0x4d, 256 / 32);
    bp(0x4b, os::native_to_physical(destination) >> 5);
    bp(0x52, (2u << 15) | (6u << 4));
    GXDrawDone();
}

// What materializing the copy writes: the fake's readback pattern, stored as the CPU encoder does.
std::array<std::uint8_t, copy_bytes> stored() {
    gx::TextureCopy p;
    p.width = 16; p.height = 8; p.format = 6; p.stride = 256;
    std::vector<std::uint8_t> pixels(16 * 8 * 4);
    for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = std::uint8_t(i % 251);
    alignas(32) std::array<std::uint8_t, copy_bytes> out{};
    gx::store_texture_copy(p, {pixels.data(), pixels.size()}, out.data(), out.size());
    return out;
}

// Each request stays alive for its completion interrupt.
void aram_dma(u32 type, u32 source, u32 destination, u32 length) {
    static ARQRequest requests[16];
    static unsigned next = 0;
    ARQPostRequest(&requests[next++ % 16], 0, type, 1, source, destination, length, nullptr);
}

u32 physical(const void* pointer) { return os::native_to_physical(pointer); }

bool all(const std::uint8_t* bytes, std::size_t size, std::uint8_t value) {
    return std::all_of(bytes, bytes + size, [value](std::uint8_t b) { return b == value; });
}

int guest_main() {
    GXFifoObj fifo{};
    __GXFifoInit();
    GXInitFifoBase(&fifo, arena, 64 * 1024);
    GXSetCPUFifo(&fifo);
    GXSetGPFifo(&fifo);
    __GXPEInit();
    static u32 aram_stack[4];
    ARInit(aram_stack, 4);
    const auto expected = stored();
    auto* x = arena + (1u << 20);
    std::uint8_t* aram = ar::memory();
    const auto stats = [] { return renderer->stats().direct; };

    // MRAM -> ARAM reads the resident copy's bytes.
    std::memset(x, 0x11, copy_bytes);
    copy_to(x);
    require(stats().resident_copies == 1 && renderer->resident_in(x, copy_bytes) && all(x, copy_bytes, 0x11),
            "the copy stays on the GPU");
    aram_dma(ARQ_TYPE_MRAM_TO_ARAM, physical(x), 0x100000, copy_bytes);
    require(stats().materialized == 1 && std::equal(expected.begin(), expected.end(), aram + 0x100000) &&
                std::equal(expected.begin(), expected.end(), x) && !renderer->resident_in(x, copy_bytes),
            "a DMA reading a resident copy gets the copy's bytes");

    // ARAM -> MRAM replaces the copy: a later read gets the DMA's bytes, not the copy's.
    copy_to(x);
    std::memset(aram + 0x200000, 0x5a, copy_bytes);
    aram_dma(ARAM_DIR_ARAM_TO_MRAM, 0x200000, physical(x), copy_bytes);
    require(all(x, copy_bytes, 0x5a) && stats().dma_overwritten == 1 && !renderer->resident_in(x, copy_bytes),
            "a DMA writing over a resident copy drops it");
    aram_dma(ARQ_TYPE_MRAM_TO_ARAM, physical(x), 0x300000, copy_bytes);
    require(stats().materialized == 1 && all(aram + 0x300000, copy_bytes, 0x5a), "the dropped copy is never materialized");

    // A DMA writing part of a copy leaves the copy's other bytes.
    copy_to(x);
    aram_dma(ARAM_DIR_ARAM_TO_MRAM, 0x200000, physical(x + 256), 64);
    aram_dma(ARQ_TYPE_MRAM_TO_ARAM, physical(x), 0x400000, copy_bytes);
    const std::uint8_t* swapped = aram + 0x400000;
    require(stats().materialized == 2 && stats().dma_overwritten == 2 &&
                std::equal(expected.begin(), expected.begin() + 256, swapped) && all(swapped + 256, 64, 0x5a) &&
                std::equal(expected.begin() + 320, expected.end(), swapped + 320),
            "a DMA reading a partly written copy gets the written bytes beside the copy's others");

    // A disc read into the destination replaces the copy too.
    copy_to(x);
    DVDFileInfo file{};
    char path[] = "/a.bin";
    require(DVDOpen(path, &file) == TRUE, "DVDOpen");
    require(DVDReadPrio(&file, x, s32(copy_bytes), 0, 2) == s32(copy_bytes), "DVDReadPrio");
    require(all(x, copy_bytes, 0x77) && stats().dma_overwritten == 3 && !renderer->resident_in(x, copy_bytes),
            "a disc read over a resident copy drops it");
    aram_dma(ARQ_TYPE_MRAM_TO_ARAM, physical(x), 0x500000, copy_bytes);
    require(stats().materialized == 2 && all(aram + 0x500000, copy_bytes, 0x77), "the disc's bytes reach ARAM");

    // Transfers elsewhere leave resident copies alone.
    copy_to(x);
    auto* y = arena + (2u << 20);
    aram_dma(ARAM_DIR_ARAM_TO_MRAM, 0x200000, physical(y), copy_bytes);
    aram_dma(ARQ_TYPE_MRAM_TO_ARAM, physical(y), 0x600000, copy_bytes);
    require(stats().materialized == 2 && stats().dma_overwritten == 3 && renderer->resident_in(x, copy_bytes),
            "DMA beside a copy leaves it resident");
    require(gx_fifo::gpu_failure()[0] == 0, "no command stream failure");
    return 0;
}

std::vector<std::uint8_t> one_file_table() {
    std::vector<std::uint8_t> fst(24);
    auto put = [&](std::size_t at, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) fst[at + i] = std::uint8_t(value >> (24 - 8 * i));
    };
    put(0, 0x01000000u);
    put(8, 2);
    put(12, 0);
    put(16, 0x1000);
    put(20, copy_bytes);
    const char name[] = "a.bin";
    fst.insert(fst.end(), name, name + sizeof name);
    return fst;
}

void make_disc() {
    const char* tmp = std::getenv("TMPDIR");
    disc_root = std::string(tmp && *tmp ? tmp : "/tmp") + "/starlet-dma-XXXXXX";
    require(mkdtemp(disc_root.data()) && mkdir((disc_root + "/sys").c_str(), 0755) == 0 &&
                mkdir((disc_root + "/files").c_str(), 0755) == 0,
            "temporary disc folder");
    const auto fst = one_file_table();
    std::ofstream(disc_root + "/sys/fst.bin", std::ios::binary).write(reinterpret_cast<const char*>(fst.data()), std::streamsize(fst.size()));
    std::vector<char> boot(0x440, 0);
    std::memcpy(boot.data(), "GTST01", 6);
    std::ofstream(disc_root + "/sys/boot.bin", std::ios::binary).write(boot.data(), std::streamsize(boot.size()));
    const std::string bytes(copy_bytes, '\x77');
    std::ofstream(disc_root + "/files/a.bin", std::ios::binary) << bytes;
}

int remove_entry(const char* path, const struct stat*, int, struct FTW*) { return std::remove(path); }

}

int main() {
    constexpr std::size_t arena_size = 16 * 1024 * 1024;
    arena = static_cast<std::uint8_t*>(os::map_fixed(0x10000000, arena_size, "test arena"));
    os::set_memory_map({0x10000000});
    os::set_arena(arena, arena_size);
    const std::uintptr_t hints[] = {0x30000000, 0x20000000};
    os::set_stack_pool(8 * 1024 * 1024, 0x40000000, hints, 2);
    os::initialize_os({});
    make_disc();
    dvd::initialize({{disc_root + "/sys", {disc_root + "/files"}}});

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
    // GL work runs on the renderer's own submission thread, as in the ports: a read waits for it.
    setup.start = [] {
        renderer->adopt_thread();
        renderer->start_submission([] {}, [] {}, [] {});
        renderer->program_builder();
    };
    gx_fifo::start_gpu(setup);
    const int result = os::run_main_thread(guest_main);
    nftw(disc_root.c_str(), remove_entry, 16, FTW_DEPTH | FTW_PHYS);
    std::printf("gx_fifo dma: %s\n", result == 0 ? "passed" : "failed");
    std::fflush(nullptr);
    std::_Exit(result);
}
