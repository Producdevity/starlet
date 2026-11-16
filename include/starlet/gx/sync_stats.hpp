#pragma once

#include <string>

namespace starlet::gx::sync_stats {

// Diagnostic counters of every point where one thread blocks on another, printed every 10 s
// when STARLET_GX_SYNC_STATS=1. A cause is a string literal; the blocked thread is named at
// the call. Everything is a no-op, one predictable branch, when the variable is unset.
bool enabled();

// The cause a wait entered below attributes to. Set by the caller of a generic wait
// (gpu drain, GL execute); the innermost one wins.
struct Context {
    explicit Context(const char* cause);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    const char* previous;
};
const char* context(const char* fallback);

// Measures the scope's wall time as blocked time of `cause` on the calling thread.
struct Wait {
    explicit Wait(const char* cause);
    ~Wait();
    Wait(const Wait&) = delete;
    Wait& operator=(const Wait&) = delete;
    const char* cause;
    long long start;
};

// Measures the scope's wall time and, separately, the part the calling thread did not spend on
// the CPU ("<tag> blocked"): GL work that waits inside the driver.
struct Task {
    explicit Task(const char* tag);
    ~Task();
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    const char* tag;
    long long start, cpu;
};

// GL thread, after each swap: the interval since the previous one, counted by retraces (16.7 ms).
void presented();

void note(const char* cause, double ms);
// A counted event (retrace, presentation) the report divides by; prints the report every 10 s.
void tick(const char* counter);
// A line of state printed with each report (the frame-ahead setting in effect, for one).
void state(const std::string& text);

} // namespace starlet::gx::sync_stats
