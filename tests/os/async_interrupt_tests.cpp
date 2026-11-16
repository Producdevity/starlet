// Asynchronous interrupts: a guest thread that spins on state only an interrupt handler changes
// finishes once the port opts in, as it does on the console's CPU, and spins on without the opt-in.
// A host frame anywhere on its stack, or disabled interrupts, defer delivery; a thread the handler
// wakes with a better priority runs before the interrupted one goes on.
#include "starlet/os/game_memory.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/os/os_core.hpp"

#include "starlet/sdk/os.h"

#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace starlet::sdk;

// Guest code as a port registers the game's: its own section, compiled with frame records.
#if defined(__clang__)
#define GUEST __attribute__((section("starlet_test_guest"), noinline))
#else
#define GUEST __attribute__((section("starlet_test_guest"), noipa))
#endif
extern "C" const char __start_starlet_test_guest[];
extern "C" const char __stop_starlet_test_guest[];

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (condition) return;
    std::printf("FAIL: %s\n", what);
    ++failures;
}

volatile int flag = 0;      // Set only by interrupt handlers.
volatile int release = 0;   // Ends a bounded spin; set by a host thread.
volatile int seen = 0;      // What the code saw before it went on.
volatile int woken_ran = 0;
volatile int seen_errno = 0;

GUEST void spin() {
    while (!flag) {
    }
}

GUEST int poll_release() { return release; }

GUEST void spin_until_released() {
    while (!poll_release()) {
    }
}

GUEST int spin_bounded() {
    while (!flag && !release) {
    }
    return flag;
}

__attribute__((noinline)) void host_busy(int milliseconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < until) {
    }
    seen = flag;
}

GUEST void guest_calls_host(int milliseconds) {
    host_busy(milliseconds);
    spin();
}

__attribute__((noinline)) void host_calls_guest() {
    spin_until_released();
    seen = flag;
}

GUEST void guest_host_guest() {
    host_calls_guest();
    spin();
}

GUEST void spin_disabled() {
    const BOOL enabled = OSDisableInterrupts();
    while (!release) {
    }
    seen = flag;
    OSRestoreInterrupts(enabled);
}

GUEST void spin_keeping_errno() {
    errno = 1234;
    spin();
    seen = woken_ran;
    seen_errno = errno;
}

GUEST void* spin_thread(void*) {
    spin();
    return nullptr;
}

GUEST void* spin_thread_leaf(void*) {
    while (!flag) {
    }
    return nullptr;
}

void raise_after(int milliseconds, void (*handler)()) {
    std::thread([milliseconds, handler] {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        starlet::os::raise_interrupt(handler);
    }).detach();
}

void release_after(int milliseconds) {
    std::thread([milliseconds] {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        release = 1;
    }).detach();
}

void set_flag() { flag = 1; }

std::chrono::steady_clock::time_point armed;

// A handler that sets the flag 5 ms from now.
void arm() {
    flag = 0;
    release = 0;
    seen = -1;
    armed = std::chrono::steady_clock::now();
    raise_after(5, set_flag);
}

bool within(int milliseconds) { return std::chrono::steady_clock::now() - armed < std::chrono::milliseconds(milliseconds); }

int saved_stderr = -1;
std::FILE* captured = nullptr;

void capture_stderr() {
    std::fflush(stderr);
    captured = std::tmpfile();
    saved_stderr = dup(STDERR_FILENO);
    if (captured && saved_stderr >= 0) dup2(fileno(captured), STDERR_FILENO);
}

std::string captured_stderr() {
    std::fflush(stderr);
    if (!captured || saved_stderr < 0) return {};
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);
    std::string text;
    std::rewind(captured);
    char buffer[4096];
    for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, captured)) > 0;) text.append(buffer, n);
    std::fclose(captured);
    std::fputs(text.c_str(), stderr);
    return text;
}

// Without the opt-in the spin never sees the handler; the next checkpoint takes it.
void not_opted_in() {
    arm();
    release_after(200);
    seen = spin_bounded();
    expect(seen == 0, "without the opt-in the handler waits for a checkpoint");
    starlet::os::checkpoint();
    expect(flag == 1, "the checkpoint took it");
}

bool opt_in() {
    const char* checkpoint = reinterpret_cast<const char*>(&starlet::os::checkpoint);
    expect(!starlet::os::add_guest_code(checkpoint, checkpoint + 4), "a range with Starlet's code is refused");
    return starlet::os::enable_async_interrupts(20);
}

void check_spin(const char* what) {
    expect(flag == 1 && within(2000), what);
}

void check_deferred(const char* what) {
    expect(seen == 0, what);
    expect(flag == 1, "taken once only guest code remained");
}

void check_threads() {
    static OSThread thread;
    static u8 stack[0x2000];
    for (auto entry : {spin_thread, spin_thread_leaf}) {
        arm();
        OSCreateThread(&thread, entry, nullptr, stack + sizeof stack, sizeof stack, 20, 0);
        OSResumeThread(&thread);
        OSJoinThread(&thread, nullptr);  // The thread spins while this one sleeps.
        check_spin("a spinning thread of its own takes the interrupt");
    }
}

OSThread woken;
u8 woken_stack[0x2000];
OSThreadQueue woken_queue;

void arm_hand_over() {
    woken_ran = 0;
    OSInitThreadQueue(&woken_queue);
    OSCreateThread(&woken, [](void*) -> void* {
        OSSleepThread(&woken_queue);
        errno = 5;
        woken_ran = 1;
        return nullptr;
    }, nullptr, woken_stack + sizeof woken_stack, sizeof woken_stack, 10, 0);
    OSResumeThread(&woken);  // Runs at once and sleeps.
    flag = 0;
    armed = std::chrono::steady_clock::now();
    raise_after(5, [] {
        flag = 1;
        OSWakeupThread(&woken_queue);
    });
}

void check_hand_over() {
    expect(seen == 1, "the thread the handler woke ran before the interrupted one went on");
    expect(seen_errno == 1234, "the interrupted thread's errno kept");
    OSJoinThread(&woken, nullptr);
}

void check_log(const std::string& log) {
    std::set<std::string> pcs;
    int taken = 0, other_thread = 0, outside = 0, repeated = 0;
    const std::string taken_text = "[os] took interrupts asynchronously in guest thread ";
    for (std::size_t at = log.find(taken_text); at != std::string::npos; at = log.find(taken_text, at + 1)) {
        ++taken;
        const std::string line = log.substr(at, log.find('\n', at) - at);
        if (line.find("guest20") != std::string::npos) ++other_thread;
        const auto pc_at = line.find(" at PC 0x");
        const auto pc = pc_at == std::string::npos ? 0 : std::strtoull(line.c_str() + pc_at + 7, nullptr, 16);
        if (pc < reinterpret_cast<std::uintptr_t>(__start_starlet_test_guest) ||
            pc >= reinterpret_cast<std::uintptr_t>(__stop_starlet_test_guest))
            ++outside;
        if (!pcs.insert(line.substr(pc_at)).second) ++repeated;
        expect(line.find("+0x") != std::string::npos, "the log names the module offset");
    }
    expect(taken > 0, "the log names where interrupts were taken");
    expect(other_thread > 0, "and in which thread");
    expect(outside == 0, "only in guest code");
    expect(repeated == 0, "once per program counter");
    int stalls = 0;
    for (std::size_t at = log.find("[os] no safe point in 1 s"); at != std::string::npos; at = log.find("[os] no safe point in 1 s", at + 1))
        ++stalls;
    expect(stalls == 1, "one report of the stall without a safe point");
    expect(log.find(": running host code\n  pc ") != std::string::npos, "it names why and where");
    expect(log.find("\n  frame ") != std::string::npos, "with the return addresses");
    expect(log.find("holds Starlet's own code; not registered") != std::string::npos, "the refused range is logged");
}

GUEST int guest_main() {
    capture_stderr();
    not_opted_in();
    if (!opt_in()) {
        captured_stderr();
        std::printf("async_interrupts: unsupported on this build; the opt-in tests are skipped\n");
        return failures;
    }
    // Only guest frames: guest_main, then spin.
    for (int round = 0; round < 6; ++round) {
        arm();
        spin();
        check_spin("the spinning main thread takes the interrupt");
    }
    check_threads();
    arm_hand_over();
    spin_keeping_errno();
    check_hand_over();
    arm();
    guest_calls_host(150);
    check_deferred("a host function the guest code called defers delivery");
    arm();
    release_after(150);
    guest_host_guest();
    check_deferred("a host frame below guest code defers delivery");
    arm();
    release_after(100);
    spin_disabled();
    check_deferred("disabled interrupts defer delivery");
    arm();
    guest_calls_host(1300);
    check_deferred("a long host function defers delivery");
    check_log(captured_stderr());
    return failures;
}

}

int main() {
    static std::vector<unsigned char> arena(std::size_t{4} << 20);
    starlet::os::set_arena(arena.data(), arena.size());
    const std::uintptr_t hints[] = {0x10000000, 0x20000000, 0x30000000};
    starlet::os::set_stack_pool(std::size_t{64} << 20, 0x40000000, hints, 3);
    starlet::os::initialize_os({});
    starlet::os::add_guest_code(__start_starlet_test_guest, __stop_starlet_test_guest);
    const int result = starlet::os::run_main_thread(guest_main);
    std::printf("%s\n", result ? "async_interrupts: failures" : "async_interrupts: ok");
    std::fflush(stdout);
    std::_Exit(result ? 1 : 0);
}
