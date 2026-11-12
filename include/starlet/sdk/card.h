#pragma once

// The Dolphin SDK's CARD calls that starlet::card_sdk implements on starlet::card and
// starlet::os, with the console SDK's layouts. A port's decompilation declares the same
// types; it asserts at compile time that its CARDFileInfo and CARDStat match these.
// Functions have C linkage, so the game's calls bind to them by name.

#include "starlet/sdk/os.h"

namespace starlet::sdk {

constexpr s32 CARD_RESULT_UNLOCKED = 1;
constexpr s32 CARD_RESULT_READY = 0;
constexpr s32 CARD_RESULT_BUSY = -1;
constexpr s32 CARD_RESULT_WRONGDEVICE = -2;
constexpr s32 CARD_RESULT_NOCARD = -3;
constexpr s32 CARD_RESULT_NOFILE = -4;
constexpr s32 CARD_RESULT_IOERROR = -5;
constexpr s32 CARD_RESULT_BROKEN = -6;
constexpr s32 CARD_RESULT_EXIST = -7;
constexpr s32 CARD_RESULT_NOENT = -8;
constexpr s32 CARD_RESULT_INSSPACE = -9;
constexpr s32 CARD_RESULT_NOPERM = -10;
constexpr s32 CARD_RESULT_LIMIT = -11;
constexpr s32 CARD_RESULT_NAMETOOLONG = -12;
constexpr s32 CARD_RESULT_ENCODING = -13;
constexpr s32 CARD_RESULT_CANCELED = -14;
constexpr s32 CARD_RESULT_FATAL_ERROR = -128;

constexpr int CARD_FILENAME_MAX = 32;
constexpr int CARD_ICON_MAX = 8;
constexpr int CARD_MAX_FILE = 127;

constexpr u8 CARD_ATTR_PUBLIC = 0x04;
constexpr u8 CARD_ATTR_NO_COPY = 0x08;
constexpr u8 CARD_ATTR_NO_MOVE = 0x10;
constexpr u8 CARD_ATTR_GLOBAL = 0x20;
constexpr u8 CARD_ATTR_COMPANY = 0x40;

constexpr u8 CARD_STAT_BANNER_NONE = 0;
constexpr u8 CARD_STAT_BANNER_C8 = 1;
constexpr u8 CARD_STAT_BANNER_RGB5A3 = 2;
constexpr u8 CARD_STAT_ANIM_LOOP = 0x00;
constexpr u8 CARD_STAT_ANIM_BOUNCE = 0x04;
constexpr u16 CARD_STAT_ICON_NONE = 0;
constexpr u16 CARD_STAT_ICON_C8 = 1;
constexpr u16 CARD_STAT_ICON_RGB5A3 = 2;
constexpr u16 CARD_STAT_SPEED_END = 0;
constexpr u16 CARD_STAT_SPEED_FAST = 1;
constexpr u16 CARD_STAT_SPEED_MIDDLE = 2;
constexpr u16 CARD_STAT_SPEED_SLOW = 3;

using CARDCallback = void (*)(s32 channel, s32 result);

struct CARDFileInfo {
    s32 chan;
    s32 fileNo;
    s32 offset;
    s32 length;
    u16 iBlock;
};

struct CARDStat {
    char fileName[CARD_FILENAME_MAX];
    u32 length;
    u32 time;  // Seconds since 2000-01-01 00:00 local time.
    u8 gameName[4];
    u8 company[2];
    u8 bannerFormat;
    u32 iconAddr;
    u16 iconFormat;
    u16 iconSpeed;
    u32 commentAddr;
    u32 offsetBanner;
    u32 offsetBannerTlut;
    u32 offsetIcon[CARD_ICON_MAX];
    u32 offsetIconTlut;
    u32 offsetData;
};
static_assert(sizeof(CARDStat) == 0x6C, "CARDStat");

// The SDK's CARDStat accessor macros.
inline u8 CARDGetBannerFormat(const CARDStat* s) { return u8(s->bannerFormat & 3); }
inline void CARDSetBannerFormat(CARDStat* s, u8 f) { s->bannerFormat = u8((s->bannerFormat & ~3) | f); }
inline u8 CARDGetIconAnim(const CARDStat* s) { return u8(s->bannerFormat & 4); }
inline void CARDSetIconAnim(CARDStat* s, u8 f) { s->bannerFormat = u8((s->bannerFormat & ~4) | f); }
inline u16 CARDGetIconFormat(const CARDStat* s, int n) { return u16((s->iconFormat >> (2 * n)) & 3); }
inline void CARDSetIconFormat(CARDStat* s, int n, u16 f) { s->iconFormat = u16((s->iconFormat & ~(3 << (2 * n))) | (f << (2 * n))); }
inline u16 CARDGetIconSpeed(const CARDStat* s, int n) { return u16((s->iconSpeed >> (2 * n)) & 3); }
inline void CARDSetIconSpeed(CARDStat* s, int n, u16 f) { s->iconSpeed = u16((s->iconSpeed & ~(3 << (2 * n))) | (f << (2 * n))); }
inline void CARDSetIconAddress(CARDStat* s, u32 a) { s->iconAddr = a; }
inline void CARDSetCommentAddress(CARDStat* s, u32 a) { s->commentAddr = a; }
inline u32 CARDGetCommentAddress(const CARDStat* s) { return s->commentAddr; }

}

#ifndef STARLET_SDK_TYPES_ONLY
extern "C" {
void CARDInit();
starlet::sdk::BOOL CARDProbe(starlet::sdk::s32 chan);
starlet::sdk::s32 CARDProbeEx(starlet::sdk::s32 chan, starlet::sdk::s32* memSize, starlet::sdk::s32* sectorSize);
// The work area belongs to the SDK's own card code; this card keeps its state on the host.
starlet::sdk::s32 CARDMountAsync(starlet::sdk::s32 chan, void* workArea, starlet::sdk::CARDCallback detachCallback,
                                 starlet::sdk::CARDCallback attachCallback);
starlet::sdk::s32 CARDMount(starlet::sdk::s32 chan, void* workArea, starlet::sdk::CARDCallback detachCallback);
starlet::sdk::s32 CARDUnmount(starlet::sdk::s32 chan);
starlet::sdk::s32 CARDCheckExAsync(starlet::sdk::s32 chan, starlet::sdk::s32* xferBytes, starlet::sdk::CARDCallback callback);
starlet::sdk::s32 CARDCheckAsync(starlet::sdk::s32 chan, starlet::sdk::CARDCallback callback);
starlet::sdk::s32 CARDCheck(starlet::sdk::s32 chan);
starlet::sdk::s32 CARDFormat(starlet::sdk::s32 chan);
starlet::sdk::s32 CARDFreeBlocks(starlet::sdk::s32 chan, starlet::sdk::s32* byteNotUsed, starlet::sdk::s32* filesNotUsed);
starlet::sdk::s32 CARDGetSerialNo(starlet::sdk::s32 chan, starlet::sdk::u64* serialNo);
starlet::sdk::s32 CARDGetSectorSize(starlet::sdk::s32 chan, starlet::sdk::u32* size);
starlet::sdk::s32 CARDOpen(starlet::sdk::s32 chan, const char* fileName, starlet::sdk::CARDFileInfo* fileInfo);
starlet::sdk::s32 CARDFastOpen(starlet::sdk::s32 chan, starlet::sdk::s32 fileNo, starlet::sdk::CARDFileInfo* fileInfo);
starlet::sdk::s32 CARDClose(starlet::sdk::CARDFileInfo* fileInfo);
starlet::sdk::s32 CARDCreateAsync(starlet::sdk::s32 chan, const char* fileName, starlet::sdk::u32 size,
                                  starlet::sdk::CARDFileInfo* fileInfo, starlet::sdk::CARDCallback callback);
starlet::sdk::s32 CARDCreate(starlet::sdk::s32 chan, const char* fileName, starlet::sdk::u32 size, starlet::sdk::CARDFileInfo* fileInfo);
starlet::sdk::s32 CARDReadAsync(starlet::sdk::CARDFileInfo* fileInfo, void* addr, starlet::sdk::s32 length, starlet::sdk::s32 offset,
                                starlet::sdk::CARDCallback callback);
starlet::sdk::s32 CARDRead(starlet::sdk::CARDFileInfo* fileInfo, void* addr, starlet::sdk::s32 length, starlet::sdk::s32 offset);
starlet::sdk::s32 CARDWriteAsync(starlet::sdk::CARDFileInfo* fileInfo, const void* addr, starlet::sdk::s32 length,
                                 starlet::sdk::s32 offset, starlet::sdk::CARDCallback callback);
starlet::sdk::s32 CARDWrite(starlet::sdk::CARDFileInfo* fileInfo, const void* addr, starlet::sdk::s32 length, starlet::sdk::s32 offset);
starlet::sdk::s32 CARDGetStatus(starlet::sdk::s32 chan, starlet::sdk::s32 fileNo, starlet::sdk::CARDStat* stat);
starlet::sdk::s32 CARDSetStatusAsync(starlet::sdk::s32 chan, starlet::sdk::s32 fileNo, starlet::sdk::CARDStat* stat,
                                     starlet::sdk::CARDCallback callback);
starlet::sdk::s32 CARDSetStatus(starlet::sdk::s32 chan, starlet::sdk::s32 fileNo, starlet::sdk::CARDStat* stat);
starlet::sdk::s32 CARDGetAttributes(starlet::sdk::s32 chan, starlet::sdk::s32 fileNo, starlet::sdk::u8* attr);
starlet::sdk::s32 CARDSetAttributesAsync(starlet::sdk::s32 chan, starlet::sdk::s32 fileNo, starlet::sdk::u8 attr,
                                         starlet::sdk::CARDCallback callback);
starlet::sdk::s32 CARDSetAttributes(starlet::sdk::s32 chan, starlet::sdk::s32 fileNo, starlet::sdk::u8 attr);
starlet::sdk::s32 CARDDeleteAsync(starlet::sdk::s32 chan, const char* fileName, starlet::sdk::CARDCallback callback);
starlet::sdk::s32 CARDDelete(starlet::sdk::s32 chan, const char* fileName);
starlet::sdk::s32 CARDFastDeleteAsync(starlet::sdk::s32 chan, starlet::sdk::s32 fileNo, starlet::sdk::CARDCallback callback);
starlet::sdk::s32 CARDFastDelete(starlet::sdk::s32 chan, starlet::sdk::s32 fileNo);
starlet::sdk::s32 CARDRenameAsync(starlet::sdk::s32 chan, const char* oldName, const char* newName, starlet::sdk::CARDCallback callback);
starlet::sdk::s32 CARDRename(starlet::sdk::s32 chan, const char* oldName, const char* newName);
}
#endif
