#pragma once

// The Dolphin OS types and functions starlet::os implements, with the console SDK's layouts. A port's
// decompilation declares the same types in its own headers; check_layouts.hpp lets the port assert at
// compile time that both agree. Functions have C linkage, so the game's calls bind to them by name.

#include <cstdint>

namespace starlet::sdk {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using f32 = float;
using f64 = double;
using BOOL = int;
constexpr BOOL FALSE = 0;
constexpr BOOL TRUE = 1;

using OSTime = s64;
using OSTick = u32;
using OSPriority = s32;

constexpr int OS_PRIORITY_MIN = 0;
constexpr int OS_PRIORITY_MAX = 31;
constexpr u16 OS_THREAD_STATE_NULL = 0;
constexpr u16 OS_THREAD_STATE_READY = 1;
constexpr u16 OS_THREAD_STATE_RUNNING = 2;
constexpr u16 OS_THREAD_STATE_WAITING = 4;
constexpr u16 OS_THREAD_STATE_MORIBUND = 8;
constexpr u16 OS_THREAD_ATTR_DETACH = 1;
constexpr u32 OS_THREAD_STACK_MAGIC = 0xDEADBABE;
constexpr u32 OS_CONSOLE_RETAIL1 = 0x00000001;
constexpr u32 OS_SOUND_MODE_STEREO = 1;
constexpr u32 OS_BUS_CLOCK = 162000000;
constexpr u32 OS_CORE_CLOCK = 486000000;
constexpr s64 OS_TIMER_CLOCK = OS_BUS_CLOCK / 4;
constexpr s64 OSMillisecondsToTicks(s64 ms) { return ms * (OS_TIMER_CLOCK / 1000); }

struct OSContext {
    u32 gpr[32];
    u32 cr, lr, ctr, xer;
    f64 fpr[32];
    u32 fpscr_pad;
    u32 fpscr;
    u32 srr0, srr1;
    u16 mode, state;
    u32 gqr[8];
    u32 psf_pad;
    f64 psf[32];
};

struct OSThread;
struct OSMutex;

struct OSThreadQueue {
    OSThread* head;
    OSThread* tail;
};

struct OSThreadLink {
    OSThread* next;
    OSThread* prev;
};

struct OSMutexQueue {
    OSMutex* head;
    OSMutex* tail;
};

struct OSMutexLink {
    OSMutex* next;
    OSMutex* prev;
};

struct OSThread {
    OSContext context;
    u16 state;
    u16 attr;
    s32 suspend;
    OSPriority priority;
    OSPriority base;
    void* val;
    OSThreadQueue* queue;
    OSThreadLink link;
    OSThreadQueue queueJoin;
    OSMutex* mutex;
    OSMutexQueue queueMutex;
    OSThreadLink linkActive;
    u8* stackBase;
    u32* stackEnd;
    s32 error;
    void* specific[2];
};

struct OSMutex {
    OSThreadQueue queue;
    OSThread* thread;
    s32 count;
    OSMutexLink link;
};

struct OSCond {
    OSThreadQueue queue;
};

struct OSSemaphore {
    s32 count;
    OSThreadQueue queue;
};

using OSMessage = void*;

struct OSMessageQueue {
    OSThreadQueue queueSend;
    OSThreadQueue queueReceive;
    OSMessage* msgArray;
    s32 msgCount;
    s32 firstIndex;
    s32 usedCount;
};

constexpr s32 OS_MESSAGE_NOBLOCK = 0;
constexpr s32 OS_MESSAGE_BLOCK = 1;

struct OSAlarm;
using OSAlarmHandler = void (*)(OSAlarm* alarm, OSContext* context);

struct OSAlarm {
    OSAlarmHandler handler;
    u32 tag;
    OSTime fire;
    OSAlarm* prev;
    OSAlarm* next;
    OSTime period;
    OSTime start;
};

struct OSCalendarTime {
    int sec, min, hour, mday, mon, year, wday, yday, msec, usec;
};

struct OSStopwatch {
    char* name;
    OSTime total;
    u32 hits;
    OSTime min;
    OSTime max;
    OSTime last;
    BOOL running;
};

// The boot information block at physical address 0, as the console stores it (32-bit words).
struct OSBootInfo {
    char gameName[4];
    char company[2];
    u8 diskNumber;
    u8 gameVersion;
    u8 streaming;
    u8 streamingBufSize;
    u8 padding[22];
    u32 magic;
    u32 version;
    u32 memorySize;
    u32 consoleType;
    u32 arenaLo;
    u32 arenaHi;
    u32 FSTLocation;
    u32 FSTMaxLength;
};
static_assert(sizeof(OSBootInfo) == 0x40, "the console's boot block");

struct OSResetFunctionInfo;
using OSResetFunction = BOOL (*)(BOOL final);
struct OSResetFunctionInfo {
    OSResetFunction func;
    u32 priority;
    OSResetFunctionInfo* next;
    OSResetFunctionInfo* prev;
};

constexpr u8 OS_LANG_ENGLISH = 0;

struct OSFontHeader {
    u16 fontType;
    u16 firstChar;
    u16 lastChar;
    u16 invalChar;
    u16 ascent;
    u16 descent;
    u16 width;
    u16 leading;
    u16 cellWidth;
    u16 cellHeight;
    u32 sheetSize;
    u16 sheetFormat;
    u16 sheetColumn;
    u16 sheetRow;
    u16 sheetWidth;
    u16 sheetHeight;
    u16 widthTable;
    u32 sheetImage;
    u32 sheetFullSize;
    u8 c0, c1, c2, c3;
};
constexpr u16 OS_FONT_ENCODE_ANSI = 0;
constexpr u32 OS_FONT_SIZE_ANSI = 288 + 131072;

using OSError = u16;
using OSErrorHandler = void (*)(OSError error, OSContext* context, ...);
constexpr OSError OS_ERROR_MAX = 17;

using OSThreadStartFunction = void* (*)(void*);
using OSSwitchThreadCallback = void (*)(OSThread* from, OSThread* to);

} // namespace starlet::sdk

// A port's layout check includes these headers next to its decompilation's with STARLET_SDK_TYPES_ONLY.
#ifndef STARLET_SDK_TYPES_ONLY
extern "C" {

void OSInitThreadQueue(starlet::sdk::OSThreadQueue* queue);
starlet::sdk::OSThread* OSGetCurrentThread();
starlet::sdk::BOOL OSIsThreadTerminated(starlet::sdk::OSThread* thread);
starlet::sdk::BOOL OSIsThreadSuspended(starlet::sdk::OSThread* thread);
starlet::sdk::s32 OSDisableScheduler();
starlet::sdk::s32 OSEnableScheduler();
void OSYieldThread();
starlet::sdk::BOOL OSCreateThread(starlet::sdk::OSThread* thread, starlet::sdk::OSThreadStartFunction func, void* param, void* stack, starlet::sdk::u32 stackSize,
                    starlet::sdk::OSPriority priority, starlet::sdk::u16 attr);
void OSExitThread(void* val);
void OSCancelThread(starlet::sdk::OSThread* thread);
void OSDetachThread(starlet::sdk::OSThread* thread);
starlet::sdk::s32 OSResumeThread(starlet::sdk::OSThread* thread);
starlet::sdk::s32 OSSuspendThread(starlet::sdk::OSThread* thread);
void OSSleepThread(starlet::sdk::OSThreadQueue* queue);
void OSWakeupThread(starlet::sdk::OSThreadQueue* queue);
starlet::sdk::OSPriority OSGetThreadPriority(starlet::sdk::OSThread* thread);
starlet::sdk::BOOL OSSetThreadPriority(starlet::sdk::OSThread* thread, starlet::sdk::OSPriority priority);
starlet::sdk::BOOL OSJoinThread(starlet::sdk::OSThread* thread, void** val);
starlet::sdk::BOOL OSDisableInterrupts();
starlet::sdk::BOOL OSEnableInterrupts();
starlet::sdk::BOOL OSRestoreInterrupts(starlet::sdk::BOOL level);
void OSInitMutex(starlet::sdk::OSMutex* mutex);
void OSLockMutex(starlet::sdk::OSMutex* mutex);
void OSUnlockMutex(starlet::sdk::OSMutex* mutex);
starlet::sdk::BOOL OSTryLockMutex(starlet::sdk::OSMutex* mutex);
void OSInitCond(starlet::sdk::OSCond* cond);
void OSWaitCond(starlet::sdk::OSCond* cond, starlet::sdk::OSMutex* mutex);
void OSSignalCond(starlet::sdk::OSCond* cond);
void OSInitMessageQueue(starlet::sdk::OSMessageQueue* queue, starlet::sdk::OSMessage* array, starlet::sdk::s32 count);
starlet::sdk::BOOL OSSendMessage(starlet::sdk::OSMessageQueue* queue, starlet::sdk::OSMessage message, starlet::sdk::s32 flags);
starlet::sdk::BOOL OSJamMessage(starlet::sdk::OSMessageQueue* queue, starlet::sdk::OSMessage message, starlet::sdk::s32 flags);
starlet::sdk::BOOL OSReceiveMessage(starlet::sdk::OSMessageQueue* queue, starlet::sdk::OSMessage* message, starlet::sdk::s32 flags);

void* OSGetArenaLo();
void* OSGetArenaHi();
void OSSetArenaLo(void* address);
void OSSetArenaHi(void* address);
starlet::sdk::OSTime OSGetTime();
starlet::sdk::OSTick OSGetTick();
void OSTicksToCalendarTime(starlet::sdk::OSTime ticks, starlet::sdk::OSCalendarTime* td);
starlet::sdk::OSTime OSCalendarTimeToTicks(starlet::sdk::OSCalendarTime* td);
void OSInitAlarm();
void OSCreateAlarm(starlet::sdk::OSAlarm* alarm);
void OSSetAlarm(starlet::sdk::OSAlarm* alarm, starlet::sdk::OSTime tick, starlet::sdk::OSAlarmHandler handler);
void OSSetAbsAlarm(starlet::sdk::OSAlarm* alarm, starlet::sdk::OSTime time, starlet::sdk::OSAlarmHandler handler);
void OSSetPeriodicAlarm(starlet::sdk::OSAlarm* alarm, starlet::sdk::OSTime start, starlet::sdk::OSTime period, starlet::sdk::OSAlarmHandler handler);
void OSCancelAlarm(starlet::sdk::OSAlarm* alarm);
void OSReport(const char* message, ...);
void OSPanic(const char* file, int line, const char* message, ...);
}
#endif
