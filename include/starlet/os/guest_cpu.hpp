#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace starlet::sdk {
struct OSThread;
}

namespace starlet::os {

// The console's single PowerPC core. Guest threads (OSThread) are user-mode
// contexts with their own stacks on one host thread, the guest CPU thread, and
// run one at a time, chosen by OS priority as the Dolphin OS scheduler chooses
// them. Switches happen inside OS calls: a blocking call, a wakeup that readies
// a higher-priority thread, a yield, or a checkpoint that finds an interrupt
// pending; a switch swaps stacks and registers without the kernel. Interrupt
// handlers run on the running thread when it has interrupts enabled, as the
// console's CPU takes them, and on a dedicated interrupt thread when no thread
// runs, so game code never runs concurrently with game code. The guest CPU
// thread sleeps only while no guest thread is ready; raise_interrupt and
// wakeups from host threads wake it.

// Host service threads (timers, disc, audio) queue interrupt handlers here.
// Handlers run in order with the CPU held and interrupts disabled.
void raise_interrupt(std::function<void()> handler);

// Makes the calling host thread the guest CPU thread (named guest_cpu_thread_name) and runs the
// game's main thread on it, at priority 16 as on the console, with entry on a low stack. Returns
// the entry's result once the main thread ends; other guest threads do not run after that.
int run_main_thread(int (*entry)());

// Guest code only: delivers pending interrupts and lets a higher-priority ready
// thread run. Long-running paths that make no other OS call (the GX FIFO) call it.
void checkpoint();
bool interrupt_pending();

// Guest code only: waits until an interrupt is pending, then delivers it. For game loops that
// spin on a variable an interrupt handler sets (a retrace count): the console's CPU took the
// interrupt in the middle of the loop.
void wait_for_interrupt();

// Asynchronous interrupts, off until a port opts in. The console's CPU takes an interrupt at any
// instruction while interrupts are enabled, and some game loops spin on state that only an
// interrupt handler changes. Once enabled, a watchdog thread signals (SIGURG) the guest CPU thread
// while interrupts have been pending for threshold_ms and it keeps running. The signal takes them
// as a checkpoint would, at the interrupted instruction, only if that instruction and every return
// address down to the thread's entry lie in code registered with add_guest_code; otherwise it
// returns and a later signal retries. Logs each program counter where it took interrupts once, and
// the stack of a thread that found no safe point for 1 s. AArch64 Linux only, and not under TSan:
// returns false elsewhere.
bool enable_async_interrupts(unsigned threshold_ms = 20);

// True while an asynchronous interrupt has stopped a guest thread at an arbitrary instruction, where
// it may hold game memory in registers. Guest CPU thread only.
bool guest_thread_preempted();

// Registers [begin, end) as guest code for asynchronous interrupts: code that the console's CPU may
// interrupt at any instruction, such as the game's. It keeps frame records (-fno-omit-frame-pointer),
// and between its instructions it holds no host lock and leaves no host state half-updated. A
// thread's entry function (run_main_thread's or OSCreateThread's) is one of its frames. Returns
// false, and logs, for a range that holds Starlet's own code. At most 16 ranges.
bool add_guest_code(const void* begin, const void* end);

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

constexpr const char* guest_cpu_thread_name = "guest-cpu";

// Profiler support, async-signal-safe: an id for the guest thread running on the calling host
// thread, guest_sample_base plus its priority at creation plus one (0: the interrupt thread),
// or 0 on other threads. Profiles name it "guest<priority>".
constexpr std::uint32_t guest_sample_base = 0x40000000u;
std::uint32_t guest_sample_id();

// Crash reports, async-signal-safe: a line naming the guest thread running on the calling host
// thread, if any.
void report_guest_thread();

}
