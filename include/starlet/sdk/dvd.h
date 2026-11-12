#pragma once

// The DVD SDK types and functions starlet::dvd implements, with the console SDK's layouts
// (see os.h for how a port checks them against its own headers).

#include "starlet/sdk/os.h"

namespace starlet::sdk {

struct DVDDiskID {
    char gameName[4];
    char company[2];
    u8 diskNumber;
    u8 gameVersion;
    u8 streaming;
    u8 streamingBufSize;
    u8 padding[22];
};
static_assert(sizeof(DVDDiskID) == 0x20, "the disc header's ID");

struct DVDCommandBlock;
using DVDCBCallback = void (*)(s32 result, DVDCommandBlock* block);

struct DVDCommandBlock {
    DVDCommandBlock* next;
    DVDCommandBlock* prev;
    u32 command;
    s32 state;
    u32 offset;
    u32 length;
    void* addr;
    u32 currTransferSize;
    u32 transferredSize;
    DVDDiskID* id;
    DVDCBCallback callback;
    void* userData;
};

struct DVDFileInfo;
using DVDCallback = void (*)(s32 result, DVDFileInfo* fileInfo);

struct DVDFileInfo {
    DVDCommandBlock cb;
    u32 startAddr;
    u32 length;
    DVDCallback callback;
};

struct DVDDir {
    u32 entryNum;
    u32 location;
    u32 next;
};

struct DVDDirEntry {
    u32 entryNum;
    BOOL isDir;
    char* name;
};

constexpr s32 DVD_RESULT_GOOD = 0;
constexpr s32 DVD_RESULT_FATAL_ERROR = -1;
constexpr s32 DVD_RESULT_CANCELED = -6;
constexpr s32 DVD_STATE_FATAL_ERROR = -1;
constexpr s32 DVD_STATE_END = 0;
constexpr s32 DVD_STATE_BUSY = 1;
constexpr s32 DVD_STATE_CANCELED = 10;
constexpr u32 DVD_COMMAND_READ = 1;
constexpr u32 DVD_COMMAND_CHANGE_DISK = 3;

} // namespace starlet::sdk

#ifndef STARLET_SDK_TYPES_ONLY
extern "C" {
void DVDInit();
starlet::sdk::BOOL DVDOpen(char* fileName, starlet::sdk::DVDFileInfo* fileInfo);
starlet::sdk::BOOL DVDFastOpen(starlet::sdk::s32 entryNum, starlet::sdk::DVDFileInfo* fileInfo);
starlet::sdk::BOOL DVDClose(starlet::sdk::DVDFileInfo* fileInfo);
starlet::sdk::s32 DVDConvertPathToEntrynum(char* path);
starlet::sdk::BOOL DVDFastOpenDir(starlet::sdk::s32 entryNum, starlet::sdk::DVDDir* dir);
starlet::sdk::BOOL DVDOpenDir(char* dirName, starlet::sdk::DVDDir* dir);
starlet::sdk::BOOL DVDReadDir(starlet::sdk::DVDDir* dir, starlet::sdk::DVDDirEntry* dirEntry);
starlet::sdk::BOOL DVDCloseDir(starlet::sdk::DVDDir* dir);
starlet::sdk::BOOL DVDChangeDir(char* dirName);
starlet::sdk::BOOL DVDGetCurrentDir(char* path, starlet::sdk::u32 maxLength);
starlet::sdk::BOOL DVDReadAsyncPrio(starlet::sdk::DVDFileInfo* fileInfo, void* addr, starlet::sdk::s32 length,
                                    starlet::sdk::s32 offset, starlet::sdk::DVDCallback callback, starlet::sdk::s32 prio);
starlet::sdk::s32 DVDReadPrio(starlet::sdk::DVDFileInfo* fileInfo, void* addr, starlet::sdk::s32 length,
                              starlet::sdk::s32 offset, starlet::sdk::s32 prio);
starlet::sdk::s32 DVDGetCommandBlockStatus(const starlet::sdk::DVDCommandBlock* block);
starlet::sdk::s32 DVDGetFileInfoStatus(const starlet::sdk::DVDFileInfo* fileInfo);
starlet::sdk::s32 DVDGetTransferredSize(starlet::sdk::DVDFileInfo* fileInfo);
starlet::sdk::s32 DVDGetDriveStatus();
starlet::sdk::BOOL DVDCancelAsync(starlet::sdk::DVDCommandBlock* block, starlet::sdk::DVDCBCallback callback);
starlet::sdk::s32 DVDCancel(starlet::sdk::DVDCommandBlock* block);
starlet::sdk::s32 DVDCancelAll();
starlet::sdk::DVDDiskID* DVDGetCurrentDiskID();
starlet::sdk::BOOL DVDCompareDiskID(const starlet::sdk::DVDDiskID* id1, const starlet::sdk::DVDDiskID* id2);
starlet::sdk::DVDDiskID* DVDGenerateDiskID(starlet::sdk::DVDDiskID* id, const char* game, const char* company,
                                           starlet::sdk::u8 diskNumber, starlet::sdk::u8 version);
starlet::sdk::BOOL DVDChangeDiskAsync(starlet::sdk::DVDCommandBlock* block, starlet::sdk::DVDDiskID* id,
                                      starlet::sdk::DVDCBCallback callback);
starlet::sdk::s32 DVDChangeDisk(starlet::sdk::DVDCommandBlock* block, starlet::sdk::DVDDiskID* id);
void* DVDGetFSTLocation();
}
#endif
