#pragma once

#include <cstddef>
#include <cstdint>

namespace starlet::os::fiber {

// A user-mode execution context on the guest CPU's host thread: its own stack and callee-saved
// registers, plus the per-thread state that belongs to the code running on it rather than to the
// host thread (errno, the C++ runtime's caught-exception chain, host_allocation routing, the
// profiler's sample id).
struct Context {
    void* sp = nullptr;  // Saved stack pointer while switched out.
    void* tsan = nullptr;
    int error = 0;
    void* exceptions[2] = {};
    bool guest_allocation = true;
    std::uint32_t sample_id = 0;
};

// The calling host thread's own context, which other contexts switch back to.
void adopt_current(Context& context);

// Makes context start entry(argument) on [stack, stack + size) when it is first switched to.
// entry must not return.
void prepare(Context& context, void* stack, std::size_t size, void (*entry)(void*), void* argument, const char* name);

// Forgets a context that will not run again (not the current one).
void release(Context& context);

// Saves the caller's state into from and continues to's code on to's stack. Returns when another
// context switches back to from.
void switch_to(Context& from, Context& to);

// The sample id of the context running on this host thread (0 outside guest contexts).
// Async-signal-safe.
std::uint32_t current_sample_id();

// Calls function(argument) from a frame record that marks the bottom of the code it runs, after
// storing that record's address to *frame. function is a void* (*)(void*) or an int (*)(); returns
// its result register.
std::uintptr_t call_entry(const void* function, void* argument, void** frame);

// The return address in the frame record of call_entry's callee. Async-signal-safe.
std::uintptr_t entry_return_address();

}
