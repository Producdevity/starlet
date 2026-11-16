#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace starlet::sdk {
struct OSThread;
}

namespace starlet::os {

// The console's single PowerPC core. Guest threads (OSThread) run on host
// threads, one at a time, chosen by OS priority as the Dolphin OS scheduler
// chooses them. Switches happen inside OS calls: a blocking call, a wakeup
// that readies a higher-priority thread, a yield, or a checkpoint that finds an
// interrupt pending. Interrupt handlers run on the running thread when it has
// interrupts enabled, as the console's CPU takes them, and on a dedicated
// interrupt thread when no thread runs, so game code never runs concurrently
// with game code.

// Host service threads (timers, disc, audio) queue interrupt handlers here.
// Handlers run in order with the CPU held and interrupts disabled.
void raise_interrupt(std::function<void()> handler);

// Starts the CPU with the game's main thread, at priority 16 as on the
// console, running entry on a low stack. Returns the entry's result.
int run_main_thread(int (*entry)());

// Guest code only: delivers pending interrupts and lets a higher-priority ready
// thread run. Long-running paths that make no other OS call (the GX FIFO) call it.
void checkpoint();
bool interrupt_pending();

// Guest code only: waits until an interrupt is pending, then delivers it. For game loops that
// spin on a variable an interrupt handler sets (a retrace count): the console's CPU took the
// interrupt in the middle of the loop.
void wait_for_interrupt();

// True on a guest thread (including the interrupt thread).
bool on_guest_thread();

// Interrupt handlers are running on this thread.
bool in_interrupt();

// Number of guest threads that currently exist, for diagnostics.
std::size_t guest_thread_count();

// Diagnostics: called with the scheduler locked when the CPU is handed to a thread ('h'), when that
// thread resumes ('g') and when it lets go ('x'), and around interrupt handlers the running thread
// takes ('i', 'j'). The value's low 7 bits are the thread's priority plus one (0: the interrupt
// thread). For 'x', bit 7 is set when the thread stays ready (preempted) and the bits above hold the
// thread CPU time of that turn in microseconds.
using SwitchTrace = void (*)(char kind, std::uint32_t value);
void set_switch_trace(SwitchTrace trace);

}
