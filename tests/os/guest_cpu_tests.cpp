// The guest CPU runs one thread at a time by Dolphin OS priority, switches only
// inside OS calls, and delivers interrupts on its interrupt thread.
#include "starlet/os/game_memory.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/os/os_core.hpp"

#include "starlet/sdk/os.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace starlet::sdk;

namespace {

std::string trace;
int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s (trace \"%s\")\n", what, trace.c_str());
        ++failures;
    }
}

u8 stack_a[0x2000], stack_b[0x2000], stack_c[0x2000];
OSThread thread_a, thread_b, thread_c;
OSMessageQueue queue;
OSMessage messages[4];
OSMutex mutex;
OSCond cond;
int shared = 0;

void* mark(void* letter) {
    trace += *static_cast<const char*>(letter);
    return letter;
}

// A higher-priority thread runs as soon as it is resumed; a lower one only
// once the creator blocks.
void priorities() {
    trace.clear();
    static const char a = 'a', b = 'b';
    OSCreateThread(&thread_a, mark, const_cast<char*>(&a), stack_a + sizeof stack_a, sizeof stack_a, 10, 0);
    OSCreateThread(&thread_b, mark, const_cast<char*>(&b), stack_b + sizeof stack_b, sizeof stack_b, 20, 0);
    expect(*thread_a.stackEnd == OS_THREAD_STACK_MAGIC, "stack end marked");
    OSResumeThread(&thread_b);
    trace += 'm';
    OSResumeThread(&thread_a);
    trace += 'M';
    void* result = nullptr;
    OSJoinThread(&thread_b, &result);
    OSJoinThread(&thread_a, nullptr);
    expect(trace == "maMb", "priority order");
    expect(result == &b, "join value");
}

void* echo(void*) {
    for (int i = 0; i < 3; ++i) {
        OSMessage message;
        OSReceiveMessage(&queue, &message, OS_MESSAGE_BLOCK);
        trace += static_cast<char>(reinterpret_cast<std::uintptr_t>(message));
    }
    return nullptr;
}

// A full queue blocks the sender until the lower-priority receiver drains it.
void messages_block() {
    trace.clear();
    OSInitMessageQueue(&queue, messages, 1);
    OSCreateThread(&thread_c, echo, nullptr, stack_c + sizeof stack_c, sizeof stack_c, 20, 0);
    OSResumeThread(&thread_c);
    for (char c : std::string("xyz")) OSSendMessage(&queue, reinterpret_cast<OSMessage>(std::uintptr_t(c)), OS_MESSAGE_BLOCK);
    OSJoinThread(&thread_c, nullptr);
    expect(trace == "xyz", "message order");
    expect(!OSReceiveMessage(&queue, nullptr, OS_MESSAGE_NOBLOCK), "empty queue does not block");
}

void* waiter(void*) {
    OSLockMutex(&mutex);
    while (shared == 0) OSWaitCond(&cond, &mutex);
    shared = 2;
    OSUnlockMutex(&mutex);
    return nullptr;
}

void mutex_and_condition() {
    OSInitMutex(&mutex);
    OSInitCond(&cond);
    shared = 0;
    OSCreateThread(&thread_a, waiter, nullptr, stack_a + sizeof stack_a, sizeof stack_a, 10, 0);
    OSResumeThread(&thread_a);  // Runs at once and waits on the condition.
    OSLockMutex(&mutex);
    expect(mutex.count == 1, "mutex taken after the waiter released it");
    shared = 1;
    OSSignalCond(&cond);
    OSUnlockMutex(&mutex);  // The waiter outranks this thread and finishes now.
    expect(shared == 2, "condition wakes the waiter");
    OSJoinThread(&thread_a, nullptr);
}

std::atomic<bool> handler_ran{false};

// Interrupts from host threads run on the guest CPU; a blocked thread is woken by them.
void interrupts() {
    OSInitMessageQueue(&queue, messages, 4);
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        starlet::os::raise_interrupt([] {
            handler_ran = starlet::os::in_interrupt();
            OSSendMessage(&queue, reinterpret_cast<OSMessage>(std::uintptr_t{7}), OS_MESSAGE_NOBLOCK);
        });
    }).detach();
    OSMessage message = nullptr;
    OSReceiveMessage(&queue, &message, OS_MESSAGE_BLOCK);
    expect(handler_ran, "handler ran on the interrupt thread");
    expect(message == reinterpret_cast<OSMessage>(std::uintptr_t{7}), "message from the handler");
}

OSAlarm alarm;
OSThreadQueue alarm_waiters;
volatile bool alarm_fired = false;

void alarms() {
    OSInitThreadQueue(&alarm_waiters);
    OSCreateAlarm(&alarm);
    const OSTime start = OSGetTime();
    OSSetAlarm(&alarm, OSMillisecondsToTicks(30), [](OSAlarm*, OSContext*) {
        alarm_fired = true;
        OSWakeupThread(&alarm_waiters);
    });
    const BOOL enabled = OSDisableInterrupts();
    while (!alarm_fired) OSSleepThread(&alarm_waiters);
    OSRestoreInterrupts(enabled);
    const OSTime elapsed = OSGetTime() - start;
    expect(elapsed >= OSMillisecondsToTicks(30), "alarm not early");
    expect(elapsed < OSMillisecondsToTicks(500), "alarm not late");
}

// With interrupts disabled, a pending interrupt waits for them to be enabled again.
void disabled_interrupts() {
    handler_ran = false;
    const BOOL enabled = OSDisableInterrupts();
    starlet::os::raise_interrupt([] { handler_ran = true; });
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
    while (std::chrono::steady_clock::now() < until) OSGetTime();
    expect(!handler_ran, "no delivery while disabled");
    OSRestoreInterrupts(enabled);
    expect(handler_ran, "delivery on enable");
}

// A loop on a variable an interrupt handler sets (the game's retrace waits) sleeps until the
// interrupt arrives and takes it.
void wait_for_interrupt() {
    static volatile int count = 0;
    count = 0;
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        starlet::os::raise_interrupt([] { count = count + 1; });
    }).detach();
    const OSTime start = OSGetTime();
    while (count == 0) starlet::os::wait_for_interrupt();
    expect(count == 1, "the handler ran once");
    expect(OSGetTime() - start >= OSMillisecondsToTicks(15), "waited for the interrupt");
}

int run_tests() {
    priorities();
    messages_block();
    mutex_and_condition();
    interrupts();
    alarms();
    disabled_interrupts();
    wait_for_interrupt();
    return failures;
}

}

int main() {
    static std::vector<unsigned char> arena(std::size_t{4} << 20);
    starlet::os::set_arena(arena.data(), arena.size());
    const std::uintptr_t hints[] = {0x10000000, 0x20000000, 0x30000000};
    starlet::os::set_stack_pool(std::size_t{64} << 20, 0x40000000, hints, 3);
    starlet::os::initialize_os({});
    const int result = starlet::os::run_main_thread(run_tests);
    std::printf("%s\n", result ? "guest_cpu: failures" : "guest_cpu: ok");
    std::fflush(stdout);
    std::_Exit(result ? 1 : 0);
}
