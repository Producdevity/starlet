#pragma once

// The Dolphin GX types and functions starlet::gx_fifo implements: FIFO objects, synchronization, EFB
// access and the pixel engine's CPU access modes, with the console SDK's values and layouts. The
// command emitters (GXAttr, GXTev, ...) stay the game's own SDK code. Functions have C linkage, so the
// game's calls bind to them by name.

#include "starlet/sdk/os.h"
#include "starlet/sdk/vi.h"

#include <cstddef>

namespace starlet::sdk {

using GXBool = u8;
constexpr GXBool GX_FALSE = 0;
constexpr GXBool GX_TRUE = 1;
constexpr GXBool GX_DISABLE = 0;
constexpr GXBool GX_ENABLE = 1;

constexpr u8 GX_FIFO_CMD_LOAD_BP_REG = 0x61;

// The SDK's opaque FIFO object.
struct GXFifoObj {
    u8 pad[128];
};
static_assert(sizeof(GXFifoObj) == 128, "the SDK's FIFO object");

// Its private layout: the console's field order (__GXFifoObj), with native pointers. A port's
// decompilation compiled natively lays its private struct out the same way, so its own SDK code
// (GXBeginDisplayList) and this module agree on the fields.
struct GXFifoObjPriv {
    u8* base;
    u8* top;  // base + size - 4
    u32 size;
    u32 hiWatermark;
    u32 loWatermark;
    void* rdPtr;
    void* wrPtr;
    s32 count;  // bytes between rdPtr and wrPtr
    u8 bind_cpu;
    u8 bind_gp;
};
static_assert(sizeof(GXFifoObjPriv) <= sizeof(GXFifoObj), "the private layout fits the SDK's object");
static_assert(sizeof(void*) != 4 || (offsetof(GXFifoObjPriv, rdPtr) == 0x14 && offsetof(GXFifoObjPriv, count) == 0x1C &&
                                     offsetof(GXFifoObjPriv, bind_cpu) == 0x20),
              "the console's private layout");

using GXBreakPtCallback = void (*)();
using GXDrawSyncCallback = void (*)(u16 token);
using GXDrawDoneCallback = void (*)();

enum GXCompare { GX_NEVER, GX_LESS, GX_EQUAL, GX_LEQUAL, GX_GREATER, GX_NEQUAL, GX_GEQUAL, GX_ALWAYS };
static_assert(sizeof(GXCompare) == 4, "the SDK's enums are 32-bit");
enum GXAlphaReadMode { GX_READ_00, GX_READ_FF, GX_READ_NONE };
enum GXBlendMode { GX_BM_NONE, GX_BM_BLEND, GX_BM_LOGIC, GX_BM_SUBTRACT };
enum GXBlendFactor {
    GX_BL_ZERO,
    GX_BL_ONE,
    GX_BL_SRCCLR,
    GX_BL_INVSRCCLR,
    GX_BL_SRCALPHA,
    GX_BL_INVSRCALPHA,
    GX_BL_DSTALPHA,
    GX_BL_INVDSTALPHA,
};
enum GXLogicOp {
    GX_LO_CLEAR,
    GX_LO_AND,
    GX_LO_REVAND,
    GX_LO_COPY,
    GX_LO_INVAND,
    GX_LO_NOOP,
    GX_LO_XOR,
    GX_LO_OR,
    GX_LO_NOR,
    GX_LO_EQUIV,
    GX_LO_INV,
    GX_LO_REVOR,
    GX_LO_INVCOPY,
    GX_LO_INVOR,
    GX_LO_NAND,
    GX_LO_SET,
};

struct GXRenderModeObj {
    VITVMode viTVmode;
    u16 fbWidth;
    u16 efbHeight;
    u16 xfbHeight;
    u16 viXOrigin;
    u16 viYOrigin;
    u16 viWidth;
    u16 viHeight;
    VIXFBMode xFBmode;
    u8 field_rendering;
    u8 aa;
    u8 sample_pattern[12][2];
    u8 vfilter[7];
};
static_assert(sizeof(GXRenderModeObj) == 0x3C, "the SDK's render mode");
static_assert(offsetof(GXRenderModeObj, fbWidth) == 0x04 && offsetof(GXRenderModeObj, xfbHeight) == 0x08 &&
                  offsetof(GXRenderModeObj, xFBmode) == 0x14 && offsetof(GXRenderModeObj, vfilter) == 0x32,
              "the SDK's render mode");

} // namespace starlet::sdk

extern "C" {

// GXFifo.c
void __GXFifoInit();
void __GXSaveCPUFifoAux(starlet::sdk::GXFifoObjPriv* fifo);
void GXInitFifoBase(starlet::sdk::GXFifoObj* fifo, void* base, starlet::sdk::u32 size);
void GXInitFifoPtrs(starlet::sdk::GXFifoObj* fifo, void* readPtr, void* writePtr);
void GXInitFifoLimits(starlet::sdk::GXFifoObj* fifo, starlet::sdk::u32 hiWatermark, starlet::sdk::u32 loWatermark);
void GXSetCPUFifo(starlet::sdk::GXFifoObj* fifo);
void GXSetGPFifo(starlet::sdk::GXFifoObj* fifo);
void GXSaveCPUFifo(starlet::sdk::GXFifoObj* fifo);
void GXSaveGPFifo(starlet::sdk::GXFifoObj* fifo);
starlet::sdk::GXFifoObj* GXGetCPUFifo();
starlet::sdk::GXFifoObj* GXGetGPFifo();
void GXGetGPStatus(starlet::sdk::GXBool* overhi, starlet::sdk::GXBool* underlow, starlet::sdk::GXBool* readIdle,
                   starlet::sdk::GXBool* cmdIdle, starlet::sdk::GXBool* brkpt);
void GXGetFifoStatus(starlet::sdk::GXFifoObj* fifo, starlet::sdk::GXBool* overhi, starlet::sdk::GXBool* underflow,
                     starlet::sdk::u32* fifoCount, starlet::sdk::GXBool* cpuWrite, starlet::sdk::GXBool* gpRead,
                     starlet::sdk::GXBool* fifowrap);
void GXGetFifoPtrs(starlet::sdk::GXFifoObj* fifo, void** readPtr, void** writePtr);
void* GXGetFifoBase(const starlet::sdk::GXFifoObj* fifo);
starlet::sdk::u32 GXGetFifoSize(const starlet::sdk::GXFifoObj* fifo);
void GXGetFifoLimits(const starlet::sdk::GXFifoObj* fifo, starlet::sdk::u32* hi, starlet::sdk::u32* lo);
starlet::sdk::GXBreakPtCallback GXSetBreakPtCallback(starlet::sdk::GXBreakPtCallback callback);
void GXEnableBreakPt(void* breakPt);
void GXDisableBreakPt();
starlet::sdk::OSThread* GXSetCurrentGXThread();
starlet::sdk::OSThread* GXGetCurrentGXThread();

// GXMisc.c
void GXFlush();
void GXResetWriteGatherPipe();
void GXAbortFrame();
void __GXAbort();
starlet::sdk::u16 GXReadDrawSync();
void GXSetDrawDone();
void GXWaitDrawDone();
void GXDrawDone();
starlet::sdk::GXDrawSyncCallback GXSetDrawSyncCallback(starlet::sdk::GXDrawSyncCallback callback);
starlet::sdk::GXDrawDoneCallback GXSetDrawDoneCallback(starlet::sdk::GXDrawDoneCallback callback);
void __GXPEInit();
void GXPeekARGB(starlet::sdk::u16 x, starlet::sdk::u16 y, starlet::sdk::u32* color);
void GXPeekZ(starlet::sdk::u16 x, starlet::sdk::u16 y, starlet::sdk::u32* z);
void GXPokeAlphaMode(starlet::sdk::GXCompare func, starlet::sdk::u8 threshold);
void GXPokeAlphaRead(starlet::sdk::GXAlphaReadMode mode);
void GXPokeAlphaUpdate(starlet::sdk::GXBool update);
void GXPokeBlendMode(starlet::sdk::GXBlendMode type, starlet::sdk::GXBlendFactor src, starlet::sdk::GXBlendFactor dst,
                     starlet::sdk::GXLogicOp op);
void GXPokeColorUpdate(starlet::sdk::GXBool update);
void GXPokeDither(starlet::sdk::GXBool dither);
void GXPokeDstAlpha(starlet::sdk::GXBool enable, starlet::sdk::u8 alpha);
void GXPokeZMode(starlet::sdk::GXBool compare, starlet::sdk::GXCompare func, starlet::sdk::GXBool update);
}
