#pragma once

#include <signal.h>
#include <sys/types.h>

#include <cstdint>

namespace starlet::os::async {

// The watchdog sends it with tgkill; its default action is to ignore it.
constexpr int interrupt_signal = SIGURG;

// The guest thread whose code the signal interrupted, as the signal handler sees it on the guest
// CPU thread. Async-signal-safe.
struct Interruptee {
    const char* refusal = nullptr;  // Why it cannot take interrupts now; null when it can.
    const char* name = nullptr;     // Null outside guest threads.
    std::uintptr_t stack_low = 0, stack_high = 0;
    std::uintptr_t entry_frame = 0;  // The frame record fiber::call_entry made on its stack.
};
extern const char* const nothing_pending;
Interruptee interruptee();

// Takes the pending interrupts as a checkpoint does, a hand-over to a better thread included.
void take_interrupts();

// From now on, records when interrupts become pending.
void track_pending();
// CLOCK_MONOTONIC nanoseconds since which interrupts have been pending; 0 when none are.
std::uint64_t pending_since();

// The kernel thread id of the guest CPU thread while run_main_thread runs, else 0.
pid_t guest_cpu_thread();

std::uint64_t now_ns();

}
