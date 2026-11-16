// The guest CPU runs one thread at a time by Dolphin OS priority, switches only
// inside OS calls, and takes interrupts on the running thread, or on its interrupt thread while
// no thread runs. Guest threads are contexts on one host thread.
#include "starlet/os/game_memory.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/os/host_allocation.hpp"
#include "starlet/os/os_core.hpp"

#include "starlet/sdk/os.h"

#include <atomic>
#include <cerrno>
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

// Ended threads hand their native stacks back: more thread lifetimes than the pool holds stacks
// (detached ones, as the game's task threads are, then joined ones) do not exhaust it.
void stacks_return() {
    static int ran = 0;
    ran = 0;
    for (int i = 0; i < 200; ++i) {
        OSCreateThread(&thread_c, [](void*) -> void* { ++ran; return nullptr; }, nullptr,
                       stack_c + sizeof stack_c, sizeof stack_c, 5, OS_THREAD_ATTR_DETACH);
        OSResumeThread(&thread_c);
    }
    for (int i = 0; i < 100; ++i) {
        OSCreateThread(&thread_a, [](void*) -> void* { ++ran; return nullptr; }, nullptr,
                       stack_a + sizeof stack_a, sizeof stack_a, 5, 0);
        OSResumeThread(&thread_a);
        OSJoinThread(&thread_a, nullptr);
    }
    expect(ran == 300, "300 short-lived threads ran in a pool of 63 stacks");
}

// The switch trace (set_switch_trace) names each hand-over: a message to a higher-priority
// thread preempts the sender, which stays ready and gets the CPU back when the receiver sleeps.
std::vector<std::pair<char, std::uint32_t>> switches;
void record_switch(char kind, std::uint32_t value) { switches.push_back({kind, value}); }

void* answer(void*) {
    for (;;) {
        OSMessage message;
        OSReceiveMessage(&queue, &message, OS_MESSAGE_BLOCK);
        if (!message) return nullptr;
        ++shared;
    }
}

void preempting_messages() {
    OSInitMessageQueue(&queue, messages, 4);
    shared = 0;
    OSCreateThread(&thread_a, answer, nullptr, stack_a + sizeof stack_a, sizeof stack_a, 15, 0);
    OSResumeThread(&thread_a);  // Runs at once and sleeps on the empty queue.
    starlet::os::set_switch_trace(record_switch);
    OSSendMessage(&queue, reinterpret_cast<OSMessage>(std::uintptr_t{1}), OS_MESSAGE_BLOCK);
    starlet::os::set_switch_trace(nullptr);
    expect(shared == 1, "the receiver answered before the sender went on");
    std::string kinds;
    for (const auto& s : switches) kinds += s.first;
    expect(kinds == "xhgxhg", "sender let go, receiver ran, sender resumed");
    if (kinds == "xhgxhg") {
        expect((switches[0].second & 0xff) == (17 | 0x80), "the sender stayed ready");
        expect((switches[1].second & 0x7f) == 16 && (switches[2].second & 0x7f) == 16, "the receiver got the CPU");
        expect((switches[3].second & 0xff) == 16, "the receiver slept");
        expect((switches[4].second & 0x7f) == 17 && (switches[5].second & 0x7f) == 17, "the sender got it back");
    }
    // Many such round trips, as a game's draw-sync manager makes them, keep one thread on the CPU.
    static std::atomic<int> inside{0};
    static std::atomic<bool> overlap{false};
    std::atomic<bool> stop{false};
    std::thread interrupter([&] {
        while (!stop.load()) {
            starlet::os::raise_interrupt([] {
                if (inside.fetch_add(1) != 0) overlap = true;
                inside.fetch_sub(1);
            });
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    for (int i = 0; i < 2000; ++i) {
        if (inside.fetch_add(1) != 0) overlap = true;
        inside.fetch_sub(1);
        OSSendMessage(&queue, reinterpret_cast<OSMessage>(std::uintptr_t{1}), OS_MESSAGE_BLOCK);
    }
    stop = true;
    interrupter.join();
    OSSendMessage(&queue, nullptr, OS_MESSAGE_BLOCK);
    OSJoinThread(&thread_a, nullptr);
    expect(shared == 2001, "every message answered in turn");
    expect(!overlap, "guest code never ran on two threads at once");
}

// An interrupt that arrives while a thread runs is taken on that thread, as on the console: no
// hand-over to the interrupt thread and back. A thread the handler wakes with a better priority
// runs before the interrupted one goes on.
void* note_woken(void*) {
    trace += 'w';
    return nullptr;
}

void interrupts_in_place() {
    trace.clear();
    switches.clear();
    OSInitThreadQueue(&alarm_waiters);
    OSCreateThread(&thread_b, [](void*) -> void* {
        OSSleepThread(&alarm_waiters);
        return note_woken(nullptr);
    }, nullptr, stack_b + sizeof stack_b, sizeof stack_b, 10, 0);
    OSResumeThread(&thread_b);  // Runs at once and sleeps.
    static std::thread::id handler_thread;
    static OSThread* handler_current = nullptr;
    static bool handler_in_interrupt = false;
    const BOOL enabled = OSDisableInterrupts();
    std::thread([] {
        starlet::os::raise_interrupt([] {
            handler_thread = std::this_thread::get_id();
            handler_current = OSGetCurrentThread();
            handler_in_interrupt = starlet::os::in_interrupt();
            trace += 'h';
            OSWakeupThread(&alarm_waiters);
            trace += 'H';
        });
    }).join();
    starlet::os::set_switch_trace(record_switch);
    OSRestoreInterrupts(enabled);
    trace += 'm';
    starlet::os::set_switch_trace(nullptr);
    OSJoinThread(&thread_b, nullptr);
    expect(handler_thread == std::this_thread::get_id(), "the handler ran on the interrupted thread");
    expect(handler_current == OSGetCurrentThread(), "OSGetCurrentThread in the handler names the interrupted thread");
    expect(handler_in_interrupt, "in_interrupt during the handler");
    expect(!starlet::os::in_interrupt(), "not in an interrupt after it");
    expect(trace == "hHwm", "the woken thread runs after the handler, before the interrupted thread goes on");
    std::string kinds;
    for (const auto& s : switches) kinds += s.first;
    expect(kinds == "ijxhgxhg", "no hand-over for the interrupt; one each way for the woken thread");
}

// Guest threads run on the guest CPU's host thread, each on its own stack in game memory, and keep
// their own errno and host allocation routing across switches.
std::thread::id main_host;
void* main_stack = nullptr;

void* ping_pong(void*) {
    int local = 0;
    expect(std::this_thread::get_id() == main_host, "a guest thread runs on the guest CPU thread");
    expect(&local != main_stack && starlet::os::game_visible(&local, sizeof local), "its own stack, in game memory");
    for (int i = 0; i < 1000; ++i) {
        errno = 1000 + i;
        OSMessage message;
        OSReceiveMessage(&queue, &message, OS_MESSAGE_BLOCK);
        expect(errno == 1000 + i, "errno kept across a switch");
        expect(starlet::os::allocating_for_guest(), "allocation routing kept across a switch");
        if (!message) break;
        ++shared;
    }
    return nullptr;
}

void switching() {
    int local = 0;
    main_host = std::this_thread::get_id();
    main_stack = &local;
    OSInitMessageQueue(&queue, messages, 1);
    shared = 0;
    OSCreateThread(&thread_a, ping_pong, nullptr, stack_a + sizeof stack_a, sizeof stack_a, 10, 0);
    OSResumeThread(&thread_a);
    for (int i = 0; i < 999; ++i) {
        errno = 7;
        OSSendMessage(&queue, reinterpret_cast<OSMessage>(std::uintptr_t{1}), OS_MESSAGE_BLOCK);
        expect(errno == 7, "the sender's errno kept");
        if (errno != 7) break;
    }
    OSSendMessage(&queue, nullptr, OS_MESSAGE_BLOCK);
    OSJoinThread(&thread_a, nullptr);
    expect(shared == 999, "1000 switches each way");
    expect(starlet::os::allocating_for_guest(), "the main thread's routing kept");
}

// The guest CPU thread sleeps while no guest thread is ready and an interrupt from a host thread
// wakes it, however close the two come (a lost wakeup hangs this test).
void interrupts_wake_the_cpu() {
    OSInitThreadQueue(&alarm_waiters);
    static std::atomic<int> raised{0}, published{0};
    static int handled = 0;
    static OSThread* current_in_handler = nullptr;
    handled = 0;
    raised = 0;
    published = 0;
    std::atomic<bool> stop{false};
    std::thread host([&] {
        int delay = 0;
        while (!stop.load()) {
            if (raised.load() != published.load()) {
                std::this_thread::yield();
                continue;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(delay));
            delay = (delay + 37) % 300;
            ++raised;
            starlet::os::raise_interrupt([] {
                ++handled;
                current_in_handler = OSGetCurrentThread();
                OSWakeupThread(&alarm_waiters);
            });
        }
    });
    const BOOL enabled = OSDisableInterrupts();
    for (int round = 1; round <= 500; ++round) {
        while (handled < round) OSSleepThread(&alarm_waiters);
        published.store(handled);
    }
    OSRestoreInterrupts(enabled);
    stop = true;
    host.join();
    expect(handled == 500 || handled == 501, "every interrupt woke the sleeping CPU");
    expect(current_in_handler == OSGetCurrentThread(), "the interrupt thread names the last guest thread");
}

// A thread that an interrupt readies with a better priority preempts the running one at its next
// checkpoint; a thread readied by the running one preempts it at once.
void priority_preemption() {
    trace.clear();
    OSInitThreadQueue(&alarm_waiters);
    OSCreateThread(&thread_b, [](void*) -> void* {
        OSSleepThread(&alarm_waiters);
        trace += 'H';
        return nullptr;
    }, nullptr, stack_b + sizeof stack_b, sizeof stack_b, 5, 0);
    OSResumeThread(&thread_b);  // Runs at once and sleeps.
    OSCreateThread(&thread_c, [](void*) -> void* {
        trace += 'L';
        std::thread([] { starlet::os::raise_interrupt([] { OSWakeupThread(&alarm_waiters); }); }).join();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (trace.find('H') == std::string::npos && std::chrono::steady_clock::now() < until) OSGetTime();
        trace += 'l';
        return nullptr;
    }, nullptr, stack_c + sizeof stack_c, sizeof stack_c, 20, 0);
    OSResumeThread(&thread_c);
    trace += 'm';
    OSJoinThread(&thread_c, nullptr);
    OSJoinThread(&thread_b, nullptr);
    expect(trace == "mLHl", "the woken thread preempted the busy lower one at its checkpoint");
    // Equal priority does not preempt; a yield gives way to it.
    trace.clear();
    OSCreateThread(&thread_a, mark, const_cast<char*>("a"), stack_a + sizeof stack_a, sizeof stack_a, 16, 0);
    OSResumeThread(&thread_a);
    trace += 'm';
    OSYieldThread();
    trace += 'M';
    OSJoinThread(&thread_a, nullptr);
    expect(trace == "maM", "equal priority waits for a yield");
}

int run_tests() {
    priorities();
    messages_block();
    mutex_and_condition();
    interrupts();
    alarms();
    disabled_interrupts();
    wait_for_interrupt();
    stacks_return();
    preempting_messages();
    interrupts_in_place();
    switching();
    interrupts_wake_the_cpu();
    priority_preemption();
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
