#pragma once

#include <cstddef>
#include <cstdint>

namespace starlet::os {

// Crash reports for log.txt, which players send without access to the device.
// Every function here is async-signal-safe.

// Writes text to stderr.
void report_text(const char* text);
void report_hex(std::uintptr_t value);

// One line per address: module+offset, and for the executable the function from its
// own symbol table (the package keeps .symtab and strips the debug information).
void report_frames(const char* label, const std::uintptr_t* addresses, std::size_t count);

// The game's OSReport text goes to this ring instead of the log (retail consoles
// discard it); a crash report ends with the last lines, as ASCII.
void keep_game_message(const char* text);
void report_game_messages();

// Installs handlers for SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE and SIGABRT that report the
// signal, the faulting address, pc, lr and registers, a backtrace and the executable's build ID
// under program's name, then end the process with the signal. Call once, early in main. A library
// handler installed later (SDL's) runs first and raises the signal again; the report then still
// describes the original fault.
void install_fatal_signal_report(const char* program);

// Gives the calling thread its own signal stack, so a fault from a stack overflow on it is still
// reported. install_fatal_signal_report does this for its caller.
void install_signal_stack();

// Called from the fatal signal handler with the fault's ucontext_t, after the registers, for a
// port's own checks of them. It must be async-signal-safe.
void set_fault_details(void (*details)(const void* context));

// The executable's GNU build ID as hex ("unknown" without one).
const char* build_id();

}
