#pragma once

// The Wii SDK's NAND calls that starlet::nand implements on host files, with the console SDK's
// layouts. A port's decompilation declares the same types; it asserts at compile time that its
// NANDFileInfo, NANDCommandBlock and NANDBanner match these, including this header before its own
// (the SDK defines the NAND_* constants as macros). Functions have C linkage, so the game's calls
// bind to them by name.

#include <cstdint>

namespace starlet::sdk {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;
using BOOL = int;

constexpr s32 NAND_RESULT_OK = 0;
constexpr s32 NAND_RESULT_ACCESS = -1;
constexpr s32 NAND_RESULT_ALLOC_FAILED = -2;
constexpr s32 NAND_RESULT_BUSY = -3;
constexpr s32 NAND_RESULT_CORRUPT = -4;
constexpr s32 NAND_RESULT_ECC_CRIT = -5;
constexpr s32 NAND_RESULT_EXISTS = -6;
constexpr s32 NAND_RESULT_INVALID = -8;
constexpr s32 NAND_RESULT_MAXBLOCKS = -9;
constexpr s32 NAND_RESULT_MAXFD = -10;
constexpr s32 NAND_RESULT_MAXFILES = -11;
constexpr s32 NAND_RESULT_NOEXISTS = -12;
constexpr s32 NAND_RESULT_NOTEMPTY = -13;
constexpr s32 NAND_RESULT_OPENFD = -14;
constexpr s32 NAND_RESULT_AUTHENTICATION = -15;
constexpr s32 NAND_RESULT_MAXDEPTH = -16;
constexpr s32 NAND_RESULT_UNKNOWN = -64;
constexpr s32 NAND_RESULT_FATAL_ERROR = -128;

constexpr int NAND_MAX_PATH = 64;

constexpr u32 NAND_CHECK_HOME_INSSPACE = 0x00000001;
constexpr u32 NAND_CHECK_HOME_INSINODE = 0x00000002;
constexpr u32 NAND_CHECK_SYS_INSSPACE = 0x00000004;
constexpr u32 NAND_CHECK_SYS_INSINODE = 0x00000008;

constexpr u8 NAND_ACCESS_NONE = 0;
constexpr u8 NAND_ACCESS_READ = 1;
constexpr u8 NAND_ACCESS_WRITE = 2;
constexpr u8 NAND_ACCESS_RW = 3;

constexpr int NAND_BANNER_TEXTURE_SIZE = 192 * 64 * 2;
constexpr int NAND_BANNER_ICON_SIZE = 48 * 48 * 2;
constexpr int NAND_BANNER_COMMENT_SIZE = 32;
constexpr u32 NAND_BANNER_SIGNATURE = 0x5749424E;

struct NANDFileInfo {
    s32 fileDescriptor;
    s32 origFd;
    char origPath[NAND_MAX_PATH];
    char tmpPath[NAND_MAX_PATH];
    u8 accType;
    u8 stage;
    u8 mark;
};

struct NANDCommandBlock {
    void* userData;
    void* callback;
    void* fileInfo;
    void* bytes;
    void* inodes;
    void* status;
    u32 ownerId;
    u16 groupId;
    u8 nextStage;
    u32 attr;
    u32 ownerAcc;
    u32 groupAcc;
    u32 othersAcc;
    u32 num;
    char absPath[NAND_MAX_PATH];
    u32* length;
    u32* pos;
    int state;
    void* copyBuf;
    u32 bufLength;
    u8* type;
    u32 uniqNo;
    u32 reqBlocks;
    u32 reqInodes;
    u32* answer;
    u32 homeBlocks;
    u32 homeInodes;
    u32 userBlocks;
    u32 userInodes;
    u32 workBlocks;
    u32 workInodes;
    const char** dir;
    BOOL simpleFlag;
};

struct NANDBanner {
    u32 signature;
    u32 flag;
    u16 iconSpeed;
    u8 reserved[22];
    u16 comment[2][NAND_BANNER_COMMENT_SIZE];
    u8 bannerTexture[NAND_BANNER_TEXTURE_SIZE];
    u8 iconTexture[8][NAND_BANNER_ICON_SIZE];
};

using NANDCallback = void (*)(s32 result, NANDCommandBlock* block);

} // namespace starlet::sdk

#ifndef STARLET_SDK_TYPES_ONLY
extern "C" {

starlet::sdk::s32 NANDInit();
starlet::sdk::s32 NANDGetHomeDir(char* path);
starlet::sdk::s32 NANDCreate(const char* path, starlet::sdk::u8 permission, starlet::sdk::u8 attribute);
starlet::sdk::s32 NANDPrivateCreate(const char* path, starlet::sdk::u8 permission, starlet::sdk::u8 attribute);
starlet::sdk::s32 NANDOpen(const char* path, starlet::sdk::NANDFileInfo* info, starlet::sdk::u8 access);
starlet::sdk::s32 NANDPrivateOpen(const char* path, starlet::sdk::NANDFileInfo* info, starlet::sdk::u8 access);
starlet::sdk::s32 NANDClose(starlet::sdk::NANDFileInfo* info);
starlet::sdk::s32 NANDRead(starlet::sdk::NANDFileInfo* info, void* bytes, starlet::sdk::u32 length);
starlet::sdk::s32 NANDWrite(starlet::sdk::NANDFileInfo* info, const void* bytes, starlet::sdk::u32 length);
starlet::sdk::s32 NANDSeek(starlet::sdk::NANDFileInfo* info, starlet::sdk::s32 offset, starlet::sdk::s32 whence);
starlet::sdk::s32 NANDGetLength(starlet::sdk::NANDFileInfo* info, starlet::sdk::u32* length);
starlet::sdk::s32 NANDDelete(const char* path);
starlet::sdk::s32 NANDPrivateDelete(const char* path);
starlet::sdk::s32 NANDMove(const char* path, const char* directory);
starlet::sdk::s32 NANDCheck(starlet::sdk::u32 blocks, starlet::sdk::u32 inodes, starlet::sdk::u32* answer);
void NANDSetUserData(starlet::sdk::NANDCommandBlock* block, void* data);
void* NANDGetUserData(const starlet::sdk::NANDCommandBlock* block);
starlet::sdk::s32 NANDOpenAsync(const char* path, starlet::sdk::NANDFileInfo* file, starlet::sdk::u8 access,
                                starlet::sdk::NANDCallback cb, starlet::sdk::NANDCommandBlock* block);
starlet::sdk::s32 NANDPrivateOpenAsync(const char* path, starlet::sdk::NANDFileInfo* file, starlet::sdk::u8 access,
                                       starlet::sdk::NANDCallback cb, starlet::sdk::NANDCommandBlock* block);
starlet::sdk::s32 NANDPrivateSafeOpenAsync(const char* path, starlet::sdk::NANDFileInfo* file, starlet::sdk::u8 access,
                                           void* buffer, starlet::sdk::u32 length, starlet::sdk::NANDCallback cb,
                                           starlet::sdk::NANDCommandBlock* block);
starlet::sdk::s32 NANDCloseAsync(starlet::sdk::NANDFileInfo* file, starlet::sdk::NANDCallback cb,
                                 starlet::sdk::NANDCommandBlock* block);
starlet::sdk::s32 NANDSafeCloseAsync(starlet::sdk::NANDFileInfo* file, starlet::sdk::NANDCallback cb,
                                     starlet::sdk::NANDCommandBlock* block);
starlet::sdk::s32 NANDReadAsync(starlet::sdk::NANDFileInfo* file, void* buffer, starlet::sdk::u32 length,
                                starlet::sdk::NANDCallback cb, starlet::sdk::NANDCommandBlock* block);
starlet::sdk::s32 NANDWriteAsync(starlet::sdk::NANDFileInfo* file, const void* buffer, starlet::sdk::u32 length,
                                 starlet::sdk::NANDCallback cb, starlet::sdk::NANDCommandBlock* block);
starlet::sdk::s32 NANDSeekAsync(starlet::sdk::NANDFileInfo* file, starlet::sdk::s32 offset, starlet::sdk::s32 whence,
                                starlet::sdk::NANDCallback cb, starlet::sdk::NANDCommandBlock* block);
starlet::sdk::s32 NANDGetLengthAsync(starlet::sdk::NANDFileInfo* file, starlet::sdk::u32* length,
                                     starlet::sdk::NANDCallback cb, starlet::sdk::NANDCommandBlock* block);
starlet::sdk::s32 NANDPrivateCreateAsync(const char* path, starlet::sdk::u8 permission, starlet::sdk::u8 attribute,
                                         starlet::sdk::NANDCallback cb, starlet::sdk::NANDCommandBlock* block);
starlet::sdk::s32 NANDPrivateCreateDirAsync(const char* path, starlet::sdk::u8 permission, starlet::sdk::u8 attribute,
                                            starlet::sdk::NANDCallback cb, starlet::sdk::NANDCommandBlock* block);
starlet::sdk::s32 NANDPrivateDeleteAsync(const char* path, starlet::sdk::NANDCallback cb,
                                         starlet::sdk::NANDCommandBlock* block);
void NANDInitBanner(starlet::sdk::NANDBanner* banner, starlet::sdk::u32 flags, const starlet::sdk::u16* title,
                    const starlet::sdk::u16* comment);

} // extern "C"
#endif
