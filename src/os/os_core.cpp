// Dolphin OS services other than threads: arena, time, alarms, reports,
// cache maintenance, console settings and reset.
#include "starlet/os/os_core.hpp"

#include "starlet/os/crash_report.hpp"

#include "starlet/os/guest_cpu.hpp"
#include "starlet/os/host_allocation.hpp"
#include "starlet/os/game_memory.hpp"
#include "starlet/os/thread_name.hpp"

#include "starlet/sdk/os.h"

#include <execinfo.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <mutex>
#include <thread>
#include <vector>

using namespace starlet::sdk;

extern "C" {
OSTime __OSStartTime = 0;
// The console's clocks, which the IPL leaves in low memory for OS_TIMER_CLOCK.
u32 __OSBusClock = OS_BUS_CLOCK;
u32 __OSCoreClock = OS_CORE_CLOCK;
}

namespace starlet::os {
namespace {

void* arena_lo = nullptr;
void* arena_hi = nullptr;
OSBootInfo local_boot_info;
OSBootInfo* boot_info = &local_boot_info;
bool print_game_log = false;
CpuWriteObserver cpu_write_observer = nullptr;
ResetObserver reset_observer = nullptr;

using clock = std::chrono::steady_clock;
const clock::time_point epoch = clock::now();
constexpr std::int64_t ticks_per_second = 162000000 / 4;

std::int64_t to_ticks(clock::duration elapsed) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return ns / 1000000000 * ticks_per_second + ns % 1000000000 * ticks_per_second / 1000000000;
}

clock::time_point from_ticks(OSTime ticks) {
    const std::int64_t seconds = ticks / ticks_per_second;
    const std::int64_t rest = ticks % ticks_per_second;
    return epoch + std::chrono::seconds(seconds) + std::chrono::nanoseconds(rest * 1000000000 / ticks_per_second);
}

// The console's time base counts from boot; the calendar is the host's local
// time at boot plus elapsed ticks, from the year 2000 as the console counts.
OSTime calendar_base = 0;

// Alarms fire on the timer thread as interrupts, in fire order.
class AlarmService {
public:
    void set(OSAlarm* alarm, OSTime fire, OSTime period, OSAlarmHandler handler) {
        host_allocation_scope host;
        std::lock_guard<std::mutex> lock(mutex_);
        remove(alarm);
        alarm->handler = handler;
        alarm->fire = fire;
        alarm->period = period;
        alarm->start = fire;
        auto at = std::find_if(alarms_.begin(), alarms_.end(), [&](OSAlarm* a) { return a->fire > fire; });
        alarms_.insert(at, alarm);
        start();
        wake_.notify_one();
    }
    void cancel(OSAlarm* alarm) {
        std::lock_guard<std::mutex> lock(mutex_);
        remove(alarm);
        alarm->handler = nullptr;
    }
    void cancel_tag(u32 tag) {
        std::lock_guard<std::mutex> lock(mutex_);
        alarms_.erase(std::remove_if(alarms_.begin(), alarms_.end(), [&](OSAlarm* a) {
            if (a->tag != tag) return false;
            a->handler = nullptr;
            return true;
        }), alarms_.end());
    }

private:
    void remove(OSAlarm* alarm) { alarms_.erase(std::remove(alarms_.begin(), alarms_.end(), alarm), alarms_.end()); }
    void start() {
        if (started_) return;
        started_ = true;
        std::thread([this] { run(); }).detach();
    }
    void run() {
        name_thread("guest-alarms");
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            if (alarms_.empty()) {
                wake_.wait(lock);
                continue;
            }
            OSAlarm* first = alarms_.front();
            const auto due = from_ticks(first->fire);
            if (clock::now() < due) {
                wake_.wait_until(lock, due);
                continue;
            }
            alarms_.erase(alarms_.begin());
            OSAlarmHandler handler = first->handler;
            const OSTime fired = first->fire;
            if (first->period > 0) {
                first->fire = fired + first->period;
                auto at = std::find_if(alarms_.begin(), alarms_.end(), [&](OSAlarm* a) { return a->fire > first->fire; });
                alarms_.insert(at, first);
            }
            if (!handler) continue;
            // The handler runs later on the guest CPU; it is skipped if the alarm
            // was cancelled or set again in between, as the console's queue would.
            raise_interrupt([this, first, handler, fired] {
                {
                    std::lock_guard<std::mutex> check(mutex_);
                    if (first->handler != handler || (first->period == 0 && first->fire != fired)) return;
                    if (first->period == 0 && std::find(alarms_.begin(), alarms_.end(), first) != alarms_.end()) return;
                    if (first->period == 0) first->handler = nullptr;
                }
                handler(first, nullptr);
            });
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<OSAlarm*> alarms_;
    bool started_ = false;
};

AlarmService& alarms() {
    static AlarmService* service = [] {
        host_allocation_scope host;
        return new AlarmService;
    }();
    return *service;
}

std::vector<OSResetFunctionInfo*> reset_functions;

}

void initialize_os(const OsConfig& config) {
    const Arena& game = arena();
    if (!game.base) throw std::logic_error("initialize_os needs the game arena");
    arena_lo = game.base;
    arena_hi = game.base + game.size;
    if (config.low_memory) {
        auto* low = static_cast<u8*>(config.low_memory);
        boot_info = reinterpret_cast<OSBootInfo*>(low);
        // Read natively by the SDK's OS_BUS_CLOCK and OS_CORE_CLOCK.
        u32 clocks[2] = {OS_BUS_CLOCK, OS_CORE_CLOCK};
        std::memcpy(low + 0xF8, clocks, sizeof clocks);
    }
    std::memcpy(boot_info->gameName, config.game_id, 4);
    std::memcpy(boot_info->company, config.game_id + 4, 2);
    boot_info->magic = 0x0D15EA5E;
    boot_info->memorySize = 24u << 20;
    boot_info->consoleType = OS_CONSOLE_RETAIL1;
    boot_info->arenaLo = native_to_physical(arena_lo) | 0x80000000u;
    boot_info->arenaHi = native_to_physical(arena_hi) | 0x80000000u;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    // Local wall-clock seconds since 2000-01-01, read back with gmtime below.
    calendar_base = (static_cast<OSTime>(now) + local.tm_gmtoff - 946684800) * ticks_per_second;
    __OSStartTime = 0;
}
OSBootInfo* boot_info_block() { return boot_info; }
void set_game_log(bool print) { print_game_log = print; }

void set_cpu_write_observer(CpuWriteObserver observer) { cpu_write_observer = observer; }
void report_cpu_write(const void* data, std::size_t bytes) {
    if (cpu_write_observer) cpu_write_observer(data, bytes);
}
void set_reset_observer(ResetObserver observer) { reset_observer = observer; }

}

using namespace starlet::os;

extern "C" {

OSBootInfo* __OSGetBootInfo() { return boot_info; }

void* OSGetArenaLo() { return arena_lo; }
void* OSGetArenaHi() { return arena_hi; }
void OSSetArenaLo(void* address) { arena_lo = address; }
void OSSetArenaHi(void* address) { arena_hi = address; }

OSTime OSGetTime() {
    checkpoint();
    return to_ticks(clock::now() - epoch);
}

OSTick OSGetTick() { return static_cast<OSTick>(OSGetTime()); }

OSTime __OSGetSystemTime() { return OSGetTime(); }

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td) {
    const std::time_t base = 946684800;  // 2000-01-01 as a UTC time_t.
    const std::int64_t total = calendar_base + ticks;
    std::int64_t seconds = total / ticks_per_second;
    std::int64_t remainder = total % ticks_per_second;
    if (remainder < 0) {
        remainder += ticks_per_second;
        --seconds;
    }
    const std::time_t when = base + static_cast<std::time_t>(seconds);
    std::tm t{};
    gmtime_r(&when, &t);
    td->sec = t.tm_sec;
    td->min = t.tm_min;
    td->hour = t.tm_hour;
    td->mday = t.tm_mday;
    td->mon = t.tm_mon;
    td->year = t.tm_year + 1900;
    td->wday = t.tm_wday;
    td->yday = t.tm_yday;
    td->msec = static_cast<int>(remainder * 1000 / ticks_per_second);
    td->usec = static_cast<int>(remainder * 1000000 / ticks_per_second % 1000);
}

OSTime OSCalendarTimeToTicks(OSCalendarTime* td) {
    std::tm t{};
    t.tm_sec = td->sec;
    t.tm_min = td->min;
    t.tm_hour = td->hour;
    t.tm_mday = td->mday;
    t.tm_mon = td->mon;
    t.tm_year = td->year - 1900;
    const std::time_t when = timegm(&t);
    const std::int64_t seconds = static_cast<std::int64_t>(when) - 946684800;
    return seconds * ticks_per_second + std::int64_t(td->msec) * (ticks_per_second / 1000) +
           std::int64_t(td->usec) * ticks_per_second / 1000000 - calendar_base;
}

void OSInitAlarm() {}
void OSCreateAlarm(OSAlarm* alarm) {
    alarm->handler = nullptr;
    alarm->tag = 0;
}
void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler) {
    alarms().set(alarm, OSGetTime() + tick, 0, handler);
}
void OSSetAbsAlarm(OSAlarm* alarm, OSTime time, OSAlarmHandler handler) { alarms().set(alarm, time, 0, handler); }
void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler) {
    // The first expiry is the next start + n * period after now, as the SDK computes it.
    OSTime first = start + period;
    const OSTime now = OSGetTime();
    if (first < now) first += ((now - first) / period + 1) * period;
    alarms().set(alarm, first, period, handler);
}
void OSCancelAlarm(OSAlarm* alarm) { alarms().cancel(alarm); }
void OSSetAlarmTag(OSAlarm* alarm, u32 tag) { alarm->tag = tag; }
void OSCancelAlarms(u32 tag) {
    if (tag) alarms().cancel_tag(tag);
}

// Retail consoles discard OSReport. The game's text goes to the crash report's ring;
// set_game_log also prints it, with Shift-JIS bytes as '?'.
void OSVReport(const char* message, va_list args) {
    char text[512];
    std::vsnprintf(text, sizeof text, message, args);
    starlet::os::keep_game_message(text);
    if (!print_game_log) return;
    for (char* c = text; *c; ++c)
        if (static_cast<unsigned char>(*c) >= 0x7f || (static_cast<unsigned char>(*c) < 0x20 && *c != '\n')) *c = '?';
    std::fputs(text, stdout);
}

void OSReport(const char* message, ...) {
    va_list args;
    va_start(args, message);
    OSVReport(message, args);
    va_end(args);
}

void OSPanic(const char* file, int line, const char* message, ...) {
    va_list args;
    va_start(args, message);
    char text[512];
    std::vsnprintf(text, sizeof text, message, args);
    va_end(args);
    for (char* c = text; *c; ++c)
        if (static_cast<unsigned char>(*c) >= 0x7f || static_cast<unsigned char>(*c) < 0x20) *c = '?';
    std::fflush(stdout);
    std::fprintf(stderr, "starlet: the game stopped at %s:%d: %s\n", file, line, text);
    void* frames[32];
    const int count = backtrace(frames, 32);
    std::uintptr_t addresses[32];
    for (int i = 0; i < count; ++i) addresses[i] = reinterpret_cast<std::uintptr_t>(frames[i]);
    starlet::os::report_frames("  at ", addresses, std::size_t(count));
    starlet::os::report_game_messages();
    std::fflush(nullptr);
    std::abort();
}

void OSRegisterVersion(const char*) {}

u32 OSGetConsoleType() { return OS_CONSOLE_RETAIL1; }
u32 OSGetSoundMode() { return OS_SOUND_MODE_STEREO; }
void OSSetSoundMode(u32) {}
u32 OSGetProgressiveMode() { return 0; }
void OSSetProgressiveMode(u32) {}
u8 OSGetLanguage() { return OS_LANG_ENGLISH; }
void OSSetLanguage(u8) {}
u32 OSGetEuRgb60Mode() { return 0; }
void OSSetEuRgb60Mode(u32) {}

void DCInvalidateRange(void*, u32) {}
void DCFlushRange(void* addr, u32 numBytes) {
    if (cpu_write_observer) cpu_write_observer(addr, numBytes);
}
void DCStoreRange(void* addr, u32 numBytes) {
    if (cpu_write_observer) cpu_write_observer(addr, numBytes);
}
void DCFlushRangeNoSync(void* addr, u32 numBytes) {
    if (cpu_write_observer) cpu_write_observer(addr, numBytes);
}
void DCStoreRangeNoSync(void* addr, u32 numBytes) {
    if (cpu_write_observer) cpu_write_observer(addr, numBytes);
}
void DCZeroRange(void* addr, u32 numBytes) { std::memset(addr, 0, numBytes); }
void DCTouchRange(void*, u32) {}
void ICInvalidateRange(void*, u32) {}
void ICFlashInvalidate() {}

void OSRegisterResetFunction(OSResetFunctionInfo* info) {
    host_allocation_scope host;
    reset_functions.push_back(info);
}
void OSUnregisterResetFunction(OSResetFunctionInfo* info) {
    reset_functions.erase(std::remove(reset_functions.begin(), reset_functions.end(), info), reset_functions.end());
}

void OSResetSystem(int reset, u32 code, BOOL doForceMenu) {
    std::printf("OSResetSystem(%d, 0x%x, %d)\n", reset, code, doForceMenu);
    std::fflush(stdout);
    if (reset_observer) reset_observer(reset, code, doForceMenu != FALSE);
    std::fprintf(stderr, "starlet: the game requested a reset and no handler is installed\n");
    std::_Exit(1);
}

u32 OSGetResetCode() { return 0; }
BOOL OSGetResetButtonState() { return FALSE; }
BOOL OSGetResetSwitchState() { return FALSE; }

}
