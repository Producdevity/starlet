// Guest side of the GX: the write-gather pipe, FIFO objects, synchronization and EFB access. Command
// emitters (GXAttr, GXTev, ...) are the game's own SDK code; their writes land here through
// starlet/compat/gx_fifo.h.
#include "gpu_internal.hpp"

#include "starlet/gx/sync_stats.hpp"
#include "starlet/compat/gx_fifo.h"
#include "starlet/gx_fifo/frame_trace.hpp"
#include "starlet/gx_fifo/gpu.hpp"
#include "starlet/gx_fifo/video.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/os/game_memory.hpp"
#include "starlet/os/host_allocation.hpp"
#include "starlet/sdk/gx.h"

#include <algorithm>
#include <cstdio>
#include <vector>

extern "C" {
StarletGxStage starlet_gx_stage = {nullptr, nullptr};
StarletGxStateCache starlet_gx_state_cache = {};
}

using namespace starlet::sdk;

namespace starlet::gx_fifo {
namespace {

constexpr std::size_t chunk_bytes = 64 * 1024;

// The chunk the stage points into while the CPU FIFO is linked to the GPU.
std::vector<std::uint8_t>* chunk = nullptr;
GXFifoObjPriv* cpu_fifo = nullptr;
GXFifoObjPriv* gp_fifo = nullptr;
// An unlinked CPU FIFO wrapped since it was set (the console's PI FIFO wrap bit).
bool cpu_fifo_wrapped = false;
OSThread* gx_thread = nullptr;

bool linked() { return cpu_fifo == nullptr || cpu_fifo == gp_fifo; }

GXFifoObjPriv* priv(GXFifoObj* fifo) { return reinterpret_cast<GXFifoObjPriv*>(fifo); }
const GXFifoObjPriv* priv(const GXFifoObj* fifo) { return reinterpret_cast<const GXFifoObjPriv*>(fifo); }

void new_chunk() {
    os::host_allocation_scope host;
    chunk = new std::vector<std::uint8_t>(chunk_bytes);
    starlet_gx_stage.cur = chunk->data();
    starlet_gx_stage.end = chunk->data() + chunk->size();
}

// Unlinked CPU FIFO: writes land in its memory as on the console, wrapping at its end.
void target_fifo_memory() {
    starlet_gx_stage.cur = static_cast<std::uint8_t*>(cpu_fifo->wrPtr);
    starlet_gx_stage.end = cpu_fifo->top;
}

void sync_fifo_write_pointer() {
    if (!linked() && cpu_fifo) cpu_fifo->wrPtr = starlet_gx_stage.cur;
}

void update_count(GXFifoObjPriv* f) {
    f->count = s32(static_cast<u8*>(f->wrPtr) - static_cast<u8*>(f->rdPtr));
    if (f->count < 0) f->count += s32(f->size);
}

GXDrawDoneCallback draw_done_callback = nullptr;
GXDrawSyncCallback draw_sync_callback = nullptr;
GXBreakPtCallback break_point_callback = nullptr;
volatile BOOL draw_done = FALSE;
OSThreadQueue finish_queue;

void finish_interrupt() {
    draw_done = TRUE;
    if (draw_done_callback) draw_done_callback();
    OSWakeupThread(&finish_queue);
}

void raise_finish() { os::raise_interrupt(finish_interrupt); }

// The draw-sync callback running now, with the EFB snapshot of its token (-1: peeks read the live
// EFB) and the pixels it has read (x0 > x1: none yet).
struct SyncCallback {
    bool active = false;
    std::uint16_t token = 0;
    int snapshot = -1;
    int x0 = 1, y0 = 1, x1 = 0, y1 = 0;
} sync_callback;

void raise_token(std::uint16_t token, int snapshot) {
    os::raise_interrupt([token, snapshot] {
        const SyncCallback outer = sync_callback;
        sync_callback = SyncCallback{};
        sync_callback.active = true;
        sync_callback.token = token;
        sync_callback.snapshot = snapshot;
        if (draw_sync_callback) draw_sync_callback(token);
        sync_callback = outer;
        if (snapshot >= 0) gpu_post([snapshot] { gpu_release_snapshot(snapshot); });
    });
}

// EFB pokes (CPU writes into the EFB) configure the pixel engine's CPU access path. The modes are
// kept; the writes themselves (GXPokeARGB, GXPokeZ) are not provided.
struct PokeModes {
    GXCompare alpha_compare = GX_ALWAYS;
    u8 alpha_threshold = 0;
    GXAlphaReadMode alpha_read = GX_READ_FF;
    GXBool alpha_update = GX_TRUE, color_update = GX_TRUE, dither = GX_FALSE;
    GXBlendMode blend = GX_BM_NONE;
    GXBlendFactor blend_source = GX_BL_ZERO, blend_destination = GX_BL_ONE;
    GXLogicOp logic_op = GX_LO_SET;
    GXBool dst_alpha_enable = GX_FALSE;
    u8 dst_alpha = 0;
    GXBool z_compare = GX_TRUE, z_update = GX_TRUE;
    GXCompare z_function = GX_ALWAYS;
} poke;

// Command chunks handed to the GPU thread so far: peeks with none between them see the same EFB.
std::uint64_t submissions = 0;

// Peek groups (GpuSetup::sync_reads off). Peeks outside draw-sync callbacks with no command
// between them form a group, numbered in the order the groups occur in a frame (VIFlush ends a
// frame). A group's first peek has the GPU thread snapshot the EFB at that point. Its peeks are
// answered from the snapshot of the same group one frame earlier, when that group began at the
// same call site and the CPU wrote no EFB pixel in this frame; otherwise they read the EFB as of
// their own point. A snapshot covers the pixels its group read one frame earlier, widened by
// peek_margin, or the whole EFB.
constexpr int peek_groups = peek_group_slots / 2;
constexpr int peek_margin = 32;
struct PeekGroup {
    std::uint32_t frame = ~0u;
    std::uintptr_t site = 0;
    int x0 = 1, y0 = 1, x1 = 0, y1 = 0;  // the pixels read (x0 > x1: none)
};
PeekGroup peek_history[2][peek_groups];  // by frame parity
std::uint32_t frame_number = 0;
std::uint32_t poked_frame = ~0u;
// The group the last peek belonged to, and the slot whose snapshot answers it (-1: none).
struct CurrentGroup {
    std::uint32_t frame = ~0u;
    std::uint64_t submissions = 0;
    int ordinal = -1;
    int answer = -1;
    bool counted = false;
} group;

void begin_group(std::uint32_t frame, std::uintptr_t site) {
    group.ordinal = group.frame == frame ? group.ordinal + 1 : 0;
    group.frame = frame;
    group.submissions = submissions;
    group.answer = -1;
    group.counted = false;
    if (group.ordinal >= peek_groups) return;
    const PeekGroup& before = peek_history[(frame - 1) & 1][group.ordinal];
    const bool repeated = before.frame == frame - 1 && before.site == site;
    int x = 0, y = 0, width = 0, height = 0;
    if (repeated && before.x0 <= before.x1) {
        x = before.x0 - peek_margin;
        y = before.y0 - peek_margin;
        width = before.x1 - before.x0 + 1 + 2 * peek_margin;
        height = before.y1 - before.y0 + 1 + 2 * peek_margin;
    }
    PeekGroup& now = peek_history[frame & 1][group.ordinal];
    now = PeekGroup{};
    now.frame = frame;
    now.site = site;
    const int slot = int(frame & 1) * peek_groups + group.ordinal;
    gpu_post([slot, frame, x, y, width, height] { gpu_take_group_snapshot(slot, frame, x, y, width, height); });
    if (repeated && poked_frame != frame) group.answer = int((frame - 1) & 1) * peek_groups + group.ordinal;
}

// A peek of a group answered from the group's snapshot one frame earlier; false when it reads
// the EFB as of its own point instead.
bool late_peek(bool depth, int x, int y, std::uintptr_t site, std::uint32_t& value) {
    if (gpu_sync_reads()) return false;
    const std::uint32_t frame = frame_number;
    if (group.frame != frame || group.submissions != submissions) begin_group(frame, site);
    if (group.ordinal >= peek_groups) return false;
    PeekGroup& now = peek_history[frame & 1][group.ordinal];
    if (now.x0 > now.x1) {
        now.x0 = now.x1 = x;
        now.y0 = now.y1 = y;
    }
    now.x0 = std::min(now.x0, x), now.x1 = std::max(now.x1, x), now.y0 = std::min(now.y0, y), now.y1 = std::max(now.y1, y);
    if (group.answer < 0) return false;
    const int slot = group.answer;
    const std::uint32_t taken = frame - 1;
    const int snapshot = gpu_group_snapshot(slot, taken);
    if (snapshot >= 0 && gpu_peek_snapshot(snapshot, depth, x, y, value)) {
        if (!group.counted) gpu_count_late_read();
        group.counted = true;
        return true;
    }
    // Not read back yet, outside its region, or not yet taken: the GPU thread reads that
    // snapshot, waiting for it. A snapshot the renderer refused leaves the live read.
    bool answered = false;
    starlet::gx::sync_stats::Context ctx("late-peek-wait");
    gpu_call([slot, taken, depth, x, y, &value, &answered] {
        const int s = gpu_group_snapshot(slot, taken);
        if (s < 0) return;
        value = gpu_peek_snapshot_wait(s, depth, x, y);
        answered = true;
    });
    if (answered && !group.counted) gpu_count_late_read();
    group.counted = group.counted || answered;
    return answered;
}

}

void flush_stage() {
    if (!linked() || !chunk) return;
    const std::size_t used = std::size_t(starlet_gx_stage.cur - chunk->data());
    if (!used) return;
    os::host_allocation_scope host;
    chunk->resize(used);
    gpu_submit(std::move(*chunk));
    ++submissions;
    delete chunk;
    new_chunk();
}

void end_peek_frame() {
    const std::uint32_t next = ++frame_number;
    // The groups of the frame before the last one are not read again.
    for (int g = 0; g < peek_groups; ++g) {
        PeekGroup& old = peek_history[next & 1][g];
        if (old.frame == ~0u) continue;
        old = PeekGroup{};
        const int slot = int(next & 1) * peek_groups + g;
        gpu_post([slot] { gpu_release_group_snapshot(slot); });
    }
}

void note_efb_poke() {
    flush_stage();
    ++submissions;
    poked_frame = frame_number;
    gpu_post([] { gpu_note_poke(); });
}

void present(const VideoFrame& frame) {
    flush_stage();
    gpu_present(frame.frame_buffer, frame.black);
}

// A draw-sync callback reads the EFB from the snapshot its token carries (gpu.cpp
// token_interrupt), instead of waiting for everything queued since. Its first reads go to the live
// EFB and teach the GPU thread to snapshot later tokens of that value. Other peeks form groups
// (late_peek). site: the game's call, which tells groups apart.
std::uint32_t peek(bool depth, int x, int y, std::uintptr_t site) {
    std::uint32_t value = 0;
    if (sync_callback.active) {
        auto& c = sync_callback;
        if (c.x0 > c.x1) {
            c.x0 = c.x1 = x;
            c.y0 = c.y1 = y;
        }
        c.x0 = std::min(c.x0, x), c.x1 = std::max(c.x1, x), c.y0 = std::min(c.y0, y), c.y1 = std::max(c.y1, y);
        gpu_note_token_peek(c.token, c.x0, c.y0, c.x1, c.y1);
        if (const int s = c.snapshot; s >= 0) {
            if (gpu_peek_snapshot(s, depth, x, y, value)) return value;
            starlet::gx::sync_stats::Context ctx("token-peek-snapshot-wait");
            gpu_call([s, depth, x, y, &value] { value = gpu_peek_snapshot_wait(s, depth, x, y); });
            return value;
        }
    }
    flush_stage();
    if (!sync_callback.active && late_peek(depth, x, y, site, value)) return value;
    starlet::gx::sync_stats::Context ctx("live-peek");
    gpu_call([depth, x, y, &value] { value = depth ? gpu_peek_depth(x, y) : gpu_peek_color(x, y); });
    return value;
}

std::uint32_t peek_depth(int x, int y) { return peek(true, x, y, 0); }
std::uint32_t peek_color(int x, int y) { return peek(false, x, y, 0); }

int request_depth_readback() {
    flush_stage();
    int handle = -1;
    starlet::gx::sync_stats::Context ctx("depth-readback-request");
    gpu_call([&handle] { handle = gpu_request_depth_readback(); });
    return handle;
}

std::uint32_t readback_depth(int handle, int x, int y) {
    std::uint32_t value = 0;
    if (gpu_depth_readback_value(handle, x, y, value)) return value;
    starlet::gx::sync_stats::Context ctx("depth-readback-wait");
    gpu_call([handle] { gpu_wait_depth_readback(handle); });
    if (!gpu_depth_readback_value(handle, x, y, value)) throw std::out_of_range("depth readback outside the EFB");
    return value;
}

bool try_readback_depth(int handle, int x, int y, std::uint32_t& value) {
    return gpu_depth_readback_value(handle, x, y, value);
}

void release_depth_readback(int handle) {
    gpu_post([handle] { gpu_release_depth_readback(handle); });
}

}

using namespace starlet::gx_fifo;

extern "C" {

int starlet_gx_draw_list_keeps_state(const void* data, unsigned size) {
    return starlet::os::game_visible(data, size) && starlet_gx_indexed_draw_list(&starlet_gx_state_cache, data, size);
}

void starlet_gx_reserve(unsigned bytes) {
    starlet::os::checkpoint();
    if (linked()) {
        if (!chunk) new_chunk();
        if (std::size_t(starlet_gx_stage.end - starlet_gx_stage.cur) < bytes) flush_stage();
        return;
    }
    // The console's FIFO wraps from its end to its base once the GPU has read past; an unlinked
    // FIFO's reader is behind the CPU, so a wrap overwrites commands the game has not yet run, as it
    // would on the console.
    if (std::size_t(starlet_gx_stage.end - starlet_gx_stage.cur) < bytes) {
        std::fprintf(stderr, "starlet: gx: CPU FIFO at %p wrapped\n", static_cast<void*>(cpu_fifo->base));
        starlet_gx_stage.cur = cpu_fifo->base;
        cpu_fifo_wrapped = true;
    }
}

// Registrations travel with the command stream, so draws before and after see the array as the game
// meant at that point.
void starlet_gx_host_order_array(const void* base, unsigned long bytes) {
    flush_stage();
    const auto address = reinterpret_cast<std::uintptr_t>(base);
    gpu_post([address, bytes] { gpu_host_order_array(address, bytes); });
}

void starlet_gx_big_endian_array(const void* base, unsigned long bytes) {
    flush_stage();
    const auto address = reinterpret_cast<std::uintptr_t>(base);
    gpu_post([address, bytes] { gpu_big_endian_array(address, bytes); });
}

void starlet_gx_release_array(const void* base) {
    flush_stage();
    const auto address = reinterpret_cast<std::uintptr_t>(base);
    gpu_post([address] { gpu_release_array(address); });
}

void __GXFifoInit() { gx_thread = OSGetCurrentThread(); }

void GXInitFifoBase(GXFifoObj* fifo, void* base, u32 size) {
    auto* f = priv(fifo);
    f->base = static_cast<u8*>(base);
    f->top = static_cast<u8*>(base) + size - 4;
    f->size = size;
    f->count = 0;
    GXInitFifoLimits(fifo, size - 0x4000, (size >> 1) & ~31u);
    GXInitFifoPtrs(fifo, base, base);
}

void GXInitFifoPtrs(GXFifoObj* fifo, void* readPtr, void* writePtr) {
    auto* f = priv(fifo);
    f->rdPtr = readPtr;
    f->wrPtr = writePtr;
    update_count(f);
    if (f == cpu_fifo && !linked()) target_fifo_memory();
}

void GXInitFifoLimits(GXFifoObj* fifo, u32 hiWatermark, u32 loWatermark) {
    auto* f = priv(fifo);
    f->hiWatermark = hiWatermark;
    f->loWatermark = loWatermark;
}

void GXSetCPUFifo(GXFifoObj* fifo) {
    starlet_gx_state_forget(&starlet_gx_state_cache);
    flush_stage();
    sync_fifo_write_pointer();
    cpu_fifo = priv(fifo);
    cpu_fifo_wrapped = false;
    if (linked()) {
        if (!chunk) new_chunk();
        starlet_gx_stage.cur = chunk->data();
        starlet_gx_stage.end = chunk->data() + chunk->size();
    } else {
        target_fifo_memory();
    }
}

void GXSetGPFifo(GXFifoObj* fifo) {
    starlet_gx_state_forget(&starlet_gx_state_cache);
    flush_stage();
    sync_fifo_write_pointer();
    auto* f = priv(fifo);
    // A FIFO the CPU filled on its own (unlinked): the GPU now runs what lies between its read and
    // write pointers. A linked FIFO's pointers stay equal: its writes went to the GPU thread directly.
    if (f->wrPtr != f->rdPtr) {
        auto* read = static_cast<std::uint8_t*>(f->rdPtr);
        auto* write = static_cast<std::uint8_t*>(f->wrPtr);
        starlet::os::host_allocation_scope host;
        std::vector<std::uint8_t> bytes;
        if (write >= read) {
            bytes.assign(read, write);
        } else {
            bytes.assign(read, f->top);
            bytes.insert(bytes.end(), f->base, write);
        }
        gpu_submit(std::move(bytes));
        ++submissions;
        f->rdPtr = f->wrPtr;
        f->count = 0;
    }
    gp_fifo = f;
    if (!cpu_fifo) return;
    if (linked()) {
        if (!chunk) new_chunk();
        starlet_gx_stage.cur = chunk->data();
        starlet_gx_stage.end = chunk->data() + chunk->size();
    } else {
        target_fifo_memory();
    }
}

void __GXSaveCPUFifoAux(GXFifoObjPriv* fifo) {
    flush_stage();
    if (fifo == cpu_fifo) sync_fifo_write_pointer();
    update_count(fifo);
}

void GXSaveCPUFifo(GXFifoObj* fifo) { __GXSaveCPUFifoAux(priv(fifo)); }

void GXSaveGPFifo(GXFifoObj*) { flush_stage(); }

GXFifoObj* GXGetCPUFifo() { return reinterpret_cast<GXFifoObj*>(cpu_fifo); }
GXFifoObj* GXGetGPFifo() { return reinterpret_cast<GXFifoObj*>(gp_fifo); }

void GXGetFifoPtrs(GXFifoObj* fifo, void** readPtr, void** writePtr) {
    auto* f = priv(fifo);
    if (f == cpu_fifo) sync_fifo_write_pointer();
    update_count(f);
    *readPtr = f->rdPtr;
    *writePtr = f->wrPtr;
}

void* GXGetFifoBase(const GXFifoObj* fifo) { return priv(fifo)->base; }
u32 GXGetFifoSize(const GXFifoObj* fifo) { return priv(fifo)->size; }

void GXGetFifoLimits(const GXFifoObj* fifo, u32* hi, u32* lo) {
    const auto* f = priv(fifo);
    *hi = f->hiWatermark;
    *lo = f->loWatermark;
}

// A status read, as on the console: it does not wait. Games' GP-hang watchdogs call it from an alarm;
// waiting there for the GPU thread would hold back every other interrupt (audio DMA, DSP mails). The
// FIFO never reaches its watermarks.
void GXGetGPStatus(GXBool* overhi, GXBool* underlow, GXBool* readIdle, GXBool* cmdIdle, GXBool* brkpt) {
    flush_stage();
    const GXBool idle = gpu_idle() ? GX_TRUE : GX_FALSE;
    *overhi = GX_FALSE;
    *underlow = GX_TRUE;
    *readIdle = idle;
    *cmdIdle = idle;
    *brkpt = GX_FALSE;
}

// The pointers of a FIFO the GPU reads are not kept: the write-gather pipe hands linked writes to the
// GPU thread directly, so its count is what the FIFO's memory holds.
void GXGetFifoStatus(GXFifoObj* fifo, GXBool* overhi, GXBool* underflow, u32* fifoCount, GXBool* cpuWrite,
                     GXBool* gpRead, GXBool* fifowrap) {
    auto* f = priv(fifo);
    if (f == cpu_fifo) {
        flush_stage();
        sync_fifo_write_pointer();
    }
    update_count(f);
    *overhi = u32(f->count) > f->hiWatermark ? GX_TRUE : GX_FALSE;
    *underflow = u32(f->count) < f->loWatermark ? GX_TRUE : GX_FALSE;
    *fifoCount = u32(f->count);
    *cpuWrite = f == cpu_fifo ? GX_TRUE : GX_FALSE;
    *gpRead = f == gp_fifo ? GX_TRUE : GX_FALSE;
    *fifowrap = f == cpu_fifo && cpu_fifo_wrapped ? GX_TRUE : GX_FALSE;
}

GXBreakPtCallback GXSetBreakPtCallback(GXBreakPtCallback callback) {
    const BOOL enabled = OSDisableInterrupts();
    const GXBreakPtCallback previous = break_point_callback;
    break_point_callback = callback;
    OSRestoreInterrupts(enabled);
    return previous;
}
// The GPU never stops at a breakpoint: decoding runs in command order and draw-sync tokens raise their
// interrupts as they pass, so a breakpoint without a callback (Super Mario Sunshine's TDrawSyncManager
// keeps the GPU one token behind with them) only changes how far the GPU may run ahead. A breakpoint
// with a callback expects that interrupt, which is not provided.
void GXEnableBreakPt(void*) {
    if (break_point_callback) OSPanic(__FILE__, __LINE__, "GX breakpoint interrupts are not supported");
}
void GXDisableBreakPt() {}

OSThread* GXSetCurrentGXThread() {
    const BOOL enabled = OSDisableInterrupts();
    OSThread* previous = gx_thread;
    gx_thread = OSGetCurrentThread();
    OSRestoreInterrupts(enabled);
    return previous;
}

OSThread* GXGetCurrentGXThread() { return gx_thread; }

void GXFlush() {
    flush_dirty_state();
    for (int i = 0; i < 8; ++i) GX_WRITE_U32(0);
    flush_stage();
}

// Writes reach the stage directly; the pipe holds no partial burst to discard.
void GXResetWriteGatherPipe() {}

// The GPU thread cannot hang and finishes queued work on its own; watchdogs abort from an alarm, where
// waiting would hold back every other interrupt.
void GXAbortFrame() { flush_stage(); }

void __GXAbort() {
    flush_stage();
    starlet::gx::sync_stats::Context ctx("gx-abort-drain");
    gpu_drain();
}

u16 GXReadDrawSync() { return gpu_token(); }

void GXSetDrawDone() {
    frame_event('d');
    const BOOL enabled = OSDisableInterrupts();
    GX_WRITE_U8(GX_FIFO_CMD_LOAD_BP_REG);
    GX_WRITE_U32(0x45000002u);
    GXFlush();
    draw_done = FALSE;
    OSRestoreInterrupts(enabled);
}

void GXWaitDrawDone() {
    const BOOL enabled = OSDisableInterrupts();
    starlet::gx::sync_stats::Wait blocked("GXWaitDrawDone (game)");
    while (!draw_done) OSSleepThread(&finish_queue);
    OSRestoreInterrupts(enabled);
    frame_event('e');
}

void GXDrawDone() {
    GXSetDrawDone();
    GXWaitDrawDone();
}

void GXPeekZ(u16 x, u16 y, u32* z) { *z = peek(true, x, y, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))); }
void GXPeekARGB(u16 x, u16 y, u32* color) {
    *color = peek(false, x, y, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
}

GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback callback) {
    const BOOL enabled = OSDisableInterrupts();
    const GXDrawSyncCallback previous = draw_sync_callback;
    draw_sync_callback = callback;
    OSRestoreInterrupts(enabled);
    return previous;
}

GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback callback) {
    const BOOL enabled = OSDisableInterrupts();
    const GXDrawDoneCallback previous = draw_done_callback;
    draw_done_callback = callback;
    OSRestoreInterrupts(enabled);
    return previous;
}

void GXPokeAlphaMode(GXCompare func, u8 threshold) {
    poke.alpha_compare = func;
    poke.alpha_threshold = threshold;
}
void GXPokeAlphaRead(GXAlphaReadMode mode) { poke.alpha_read = mode; }
void GXPokeAlphaUpdate(GXBool update) { poke.alpha_update = update; }
void GXPokeBlendMode(GXBlendMode type, GXBlendFactor src, GXBlendFactor dst, GXLogicOp op) {
    poke.blend = type;
    poke.blend_source = src;
    poke.blend_destination = dst;
    poke.logic_op = op;
}
void GXPokeColorUpdate(GXBool update) { poke.color_update = update; }
void GXPokeDither(GXBool dither) { poke.dither = dither; }
void GXPokeDstAlpha(GXBool enable, u8 alpha) {
    poke.dst_alpha_enable = enable;
    poke.dst_alpha = alpha;
}
void GXPokeZMode(GXBool compare, GXCompare func, GXBool update) {
    poke.z_compare = compare;
    poke.z_function = func;
    poke.z_update = update;
}

void __GXPEInit() {
    OSInitThreadQueue(&finish_queue);
    GpuEvents events;
    events.draw_done = raise_finish;
    events.token = raise_token;
    set_gpu_events(events);
}

}
