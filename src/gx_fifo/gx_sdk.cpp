// Guest side of the GX: the write-gather pipe, FIFO objects, synchronization and EFB access. Command
// emitters (GXAttr, GXTev, ...) are the game's own SDK code; their writes land here through
// starlet/compat/gx_fifo.h.
#include "gpu_internal.hpp"

#include "starlet/compat/gx_fifo.h"
#include "starlet/gx_fifo/frame_trace.hpp"
#include "starlet/gx_fifo/gpu.hpp"
#include "starlet/gx_fifo/video.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/os/host_allocation.hpp"
#include "starlet/sdk/gx.h"

#include <cstdio>
#include <vector>

extern "C" {
StarletGxStage starlet_gx_stage = {nullptr, nullptr};
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

void raise_token(std::uint16_t token) {
    os::raise_interrupt([token] {
        if (draw_sync_callback) draw_sync_callback(token);
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

}

void flush_stage() {
    if (!linked() || !chunk) return;
    const std::size_t used = std::size_t(starlet_gx_stage.cur - chunk->data());
    if (!used) return;
    os::host_allocation_scope host;
    chunk->resize(used);
    gpu_submit(std::move(*chunk));
    delete chunk;
    new_chunk();
}

void present(const VideoFrame& frame) {
    flush_stage();
    gpu_present(frame.frame_buffer, frame.black);
}

std::uint32_t peek_depth(int x, int y) {
    std::uint32_t z = 0;
    flush_stage();
    gpu_call([x, y, &z] { z = gpu_peek_depth(x, y); });
    return z;
}

std::uint32_t peek_color(int x, int y) {
    std::uint32_t color = 0;
    flush_stage();
    gpu_call([x, y, &color] { color = gpu_peek_color(x, y); });
    return color;
}

}

using namespace starlet::gx_fifo;

extern "C" {

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
    while (!draw_done) OSSleepThread(&finish_queue);
    OSRestoreInterrupts(enabled);
    frame_event('e');
}

void GXDrawDone() {
    GXSetDrawDone();
    GXWaitDrawDone();
}

void GXPeekZ(u16 x, u16 y, u32* z) { *z = peek_depth(x, y); }
void GXPeekARGB(u16 x, u16 y, u32* color) { *color = peek_color(x, y); }

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
