// Remaining OS services: processor registers, exception context, error handlers, the locked cache
// switch and the IPL ROM font.
#include "starlet/os/guest_cpu.hpp"
#include "starlet/sdk/os.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace starlet::sdk;

extern "C" {

// The floating-point exceptions the game enabled; natively none trap.
u32 __OSFpscrEnableBits = 0;

void OSInit() {}

// Games read these to describe a faulting thread; the port's crash report comes from its signal
// handler instead.
static OSContext current_context;
OSContext* OSGetCurrentContext() { return &current_context; }
void OSSetCurrentContext(OSContext* context) { (void)context; }
void OSClearContext(OSContext* context) { std::memset(context, 0, sizeof *context); }
void OSFillFPUContext(OSContext*) {}

u32 OSGetStackPointer() {
    return static_cast<u32>(reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)));
}

static OSErrorHandler error_handlers[OS_ERROR_MAX];
OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler) {
    if (error >= OS_ERROR_MAX) return nullptr;
    OSErrorHandler previous = error_handlers[error];
    error_handlers[error] = handler;
    return previous;
}

// Memory protection only traps stray writes on development hardware.
void OSProtectRange(u32, void*, u32, u32) {}
void OSSetSaveRegion(void*, void*) {}
u32 OSGetConsoleSimulatedMemSize() { return 24u << 20; }

// Processor implementation registers (performance monitor, write-gather pipe, HID2) have no native
// counterpart; reads return the reset values.
u32 PPCMfhid2() { return 0; }
void PPCMthid2(u32) {}
u32 PPCMfmsr() { return 0x8000; }  // MSR[EE]
void PPCMtmsr(u32) {}
u32 PPCMfwpar() { return 0; }
void PPCMtwpar(u32) {}
void PPCMtmmcr0(u32) {}
void PPCMtmmcr1(u32) {}
void PPCMtpmc1(u32) {}
void PPCMtpmc2(u32) {}
void PPCMtpmc3(u32) {}
void PPCMtpmc4(u32) {}
u32 PPCMfpmc1() { return 0; }
void PPCSync() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

// The locked cache is memory the port maps where the game addresses it (0xE0000000).
void LCEnable() {}
void LCDisable() {}

u16 OSGetFontEncode() { return OS_FONT_ENCODE_ANSI; }

// The IPL ROM's ANSI font is console firmware a port does not have. The header describes the ROM
// font's geometry (24x24 cells, 512x512 I4 sheets) so text lays out as on the console; the sheets are
// blank. Games draw with this font for disc errors, which cannot occur natively.
BOOL OSInitFont(OSFontHeader* font) {
    static bool reported = false;
    if (!reported) {
        reported = true;
        std::printf("starlet: IPL ROM font unavailable; disc-error text would draw blank\n");
    }
    std::memset(font, 0, OS_FONT_SIZE_ANSI);
    font->fontType = OS_FONT_ENCODE_ANSI;
    font->firstChar = 0x20;
    font->lastChar = 0xff;
    font->invalChar = 0x20;
    font->ascent = 18;
    font->descent = 6;
    font->width = 24;
    font->leading = 2;
    font->cellWidth = 24;
    font->cellHeight = 24;
    font->sheetFormat = 0;  // GX_TF_I4
    font->sheetColumn = 21;
    font->sheetRow = 21;
    font->sheetWidth = 512;
    font->sheetHeight = 512;
    font->sheetSize = 512 * 512 / 2;
    font->widthTable = sizeof(OSFontHeader);
    font->sheetImage = (sizeof(OSFontHeader) + 224 + 31) & ~31u;
    font->sheetFullSize = font->sheetSize;
    auto* widths = reinterpret_cast<u8*>(font) + font->widthTable;
    std::memset(widths, 12, 224);
    return TRUE;
}

static u32 glyph_index(const char*& string) {
    const unsigned char c = static_cast<unsigned char>(*string);
    if (c) ++string;
    return c < 0x20 ? 0 : c - 0x20;
}

char* OSGetFontTexture(const char* string, void** image, s32* x, s32* y, s32* width) {
    static u8 blank_sheet[512 * 512 / 2] __attribute__((aligned(32)));
    const u32 index = glyph_index(string);
    *image = blank_sheet;
    *x = s32(index % 21) * 24;
    *y = s32(index / 21) * 24;
    if (width) *width = 12;
    return const_cast<char*>(string);
}

char* OSGetFontWidth(const char* string, s32* width) {
    glyph_index(string);
    if (width) *width = 12;
    return const_cast<char*>(string);
}

}
