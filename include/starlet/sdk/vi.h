#pragma once

// The Dolphin VI types and functions starlet::gx_fifo implements, with the console SDK's values and
// layouts. Functions have C linkage, so the game's calls bind to them by name.

#include "starlet/sdk/os.h"

namespace starlet::sdk {

constexpr u32 VI_INTERLACE = 0;
constexpr u32 VI_NON_INTERLACE = 1;
constexpr u32 VI_PROGRESSIVE = 2;

constexpr u32 VI_NTSC = 0;
constexpr u32 VI_PAL = 1;
constexpr u32 VI_MPAL = 2;
constexpr u32 VI_DEBUG = 3;
constexpr u32 VI_DEBUG_PAL = 4;
constexpr u32 VI_EURGB60 = 5;

constexpr u32 vi_tv_mode(u32 format, u32 interlace) { return (format << 2) + interlace; }

enum VITVMode {
    VI_TVMODE_NTSC_INT = vi_tv_mode(VI_NTSC, VI_INTERLACE),
    VI_TVMODE_NTSC_DS = vi_tv_mode(VI_NTSC, VI_NON_INTERLACE),
    VI_TVMODE_NTSC_PROG = vi_tv_mode(VI_NTSC, VI_PROGRESSIVE),
    VI_TVMODE_PAL_INT = vi_tv_mode(VI_PAL, VI_INTERLACE),
    VI_TVMODE_PAL_DS = vi_tv_mode(VI_PAL, VI_NON_INTERLACE),
    VI_TVMODE_EURGB60_INT = vi_tv_mode(VI_EURGB60, VI_INTERLACE),
    VI_TVMODE_EURGB60_DS = vi_tv_mode(VI_EURGB60, VI_NON_INTERLACE),
    VI_TVMODE_MPAL_INT = vi_tv_mode(VI_MPAL, VI_INTERLACE),
    VI_TVMODE_MPAL_DS = vi_tv_mode(VI_MPAL, VI_NON_INTERLACE),
    VI_TVMODE_DEBUG_INT = vi_tv_mode(VI_DEBUG, VI_INTERLACE),
    VI_TVMODE_DEBUG_PAL_INT = vi_tv_mode(VI_DEBUG_PAL, VI_INTERLACE),
    VI_TVMODE_DEBUG_PAL_DS = vi_tv_mode(VI_DEBUG_PAL, VI_NON_INTERLACE),
};
static_assert(sizeof(VITVMode) == 4, "the SDK's enums are 32-bit");

enum VIXFBMode {
    VI_XFBMODE_SF = 0,
    VI_XFBMODE_DF,
};

using VIRetraceCallback = void (*)(u32 retraceCount);

// Defined in starlet/sdk/gx.h, as the SDK's GXStruct.h defines it.
struct GXRenderModeObj;

} // namespace starlet::sdk

extern "C" {

void VIInit();
void VIConfigure(const starlet::sdk::GXRenderModeObj* rm);
void VIFlush();
void VISetNextFrameBuffer(void* fb);
void* VIGetNextFrameBuffer();
void* VIGetCurrentFrameBuffer();
void VISetBlack(starlet::sdk::BOOL black);
void VIWaitForRetrace();
starlet::sdk::u32 VIGetRetraceCount();
starlet::sdk::VIRetraceCallback VISetPreRetraceCallback(starlet::sdk::VIRetraceCallback callback);
starlet::sdk::VIRetraceCallback VISetPostRetraceCallback(starlet::sdk::VIRetraceCallback callback);
starlet::sdk::u32 VIGetNextField();
starlet::sdk::u32 VIGetCurrentLine();
starlet::sdk::u32 VIGetTvFormat();
starlet::sdk::u32 VIGetDTVStatus();
}
