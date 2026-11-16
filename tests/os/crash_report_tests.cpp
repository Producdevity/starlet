// The fatal signal report describes the original fault when another handler caught it first and
// raised the signal again: SDL's console cleanup restores the previous handler, unblocks the signal
// and raises it from inside its own handler; a plain handler's raise arrives after it returns.
// A fault on a guest thread, a stack overflow included, names that thread. An abort in an interrupt
// handler taken asynchronously is reported as itself: the interrupt's signal frame is no fault.
#include "starlet/os/crash_report.hpp"
#include "starlet/os/game_memory.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/os/os_core.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" const char __start_starlet_crash_guest[];
extern "C" const char __stop_starlet_crash_guest[];

namespace {

struct sigaction previous;
bool unblock = false;

void library_handler(int signal) {
    sigaction(signal, &previous, nullptr);
    if (unblock) {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, signal);
        sigprocmask(SIG_UNBLOCK, &set, nullptr);
    }
    raise(signal);
}

void library_action(int signal, siginfo_t*, void*) { library_handler(signal); }

void details(const void*) { (void)!write(STDERR_FILENO, "port details\n", 13); }

// SDL's handler takes a siginfo; without SA_SIGINFO the kernel leaves the frame's siginfo unset.
enum class Order { report_only, nested_raise, nested_raise_siginfo, raise_after_return };

[[noreturn]] __attribute__((noinline)) void fault(Order order) {
    starlet::os::install_fatal_signal_report("crash_test");
    starlet::os::set_fault_details(details);
    unblock = order == Order::nested_raise || order == Order::nested_raise_siginfo;
    if (order != Order::report_only) {
        struct sigaction action {};
        if (order == Order::nested_raise_siginfo) {
            action.sa_sigaction = library_action;
            action.sa_flags = SA_SIGINFO;
        } else {
            action.sa_handler = library_handler;
        }
        sigaction(SIGSEGV, &action, &previous);
    }
    *reinterpret_cast<volatile int*>(std::uintptr_t{0x1238}) = 1;
    _exit(0);
}

__attribute__((noinline)) void fault_on_guest() { *reinterpret_cast<volatile int*>(std::uintptr_t{0x1238}) = 1; }

__attribute__((noinline)) int overflow(int depth) {
    volatile char frame[512];
    frame[0] = char(depth);
    return overflow(depth + 1) + frame[0];
}

bool overflow_guest = false;

[[noreturn]] void fault_guest(bool overflowing) {
    starlet::os::install_fatal_signal_report("crash_test");
    static std::vector<unsigned char> arena(std::size_t{1} << 20);
    starlet::os::set_arena(arena.data(), arena.size());
    const std::uintptr_t hints[] = {0x10000000, 0x20000000, 0x30000000};
    starlet::os::set_stack_pool(std::size_t{16} << 20, 0x40000000, hints, 3);
    starlet::os::initialize_os({});
    overflow_guest = overflowing;
    starlet::os::run_main_thread([]() -> int {
        if (overflow_guest) return overflow(0);
        fault_on_guest();
        return 0;
    });
    _exit(0);
}

volatile int never = 0;

__attribute__((noinline)) void raise_abort() {
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        starlet::os::raise_interrupt([] { std::abort(); });
    }).detach();
}

__attribute__((section("starlet_crash_guest"), noinline)) int spin_into_abort() {
    raise_abort();
    while (!never) {
    }
    return 0;
}

[[noreturn]] void abort_in_async_handler() {
    starlet::os::install_fatal_signal_report("crash_test");
    static std::vector<unsigned char> arena(std::size_t{1} << 20);
    starlet::os::set_arena(arena.data(), arena.size());
    const std::uintptr_t hints[] = {0x10000000, 0x20000000, 0x30000000};
    starlet::os::set_stack_pool(std::size_t{16} << 20, 0x40000000, hints, 3);
    starlet::os::initialize_os({});
    starlet::os::add_guest_code(__start_starlet_crash_guest, __stop_starlet_crash_guest);
    if (!starlet::os::enable_async_interrupts(5)) _exit(0);
    starlet::os::run_main_thread(spin_into_abort);
    _exit(0);
}

template <class Child> std::string run_child(Child child, int& status) {
    int pipe_fds[2];
    if (pipe(pipe_fds)) return {};
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[0]);
        child();
    }
    close(pipe_fds[1]);
    std::string text;
    char buffer[4096];
    for (ssize_t n; (n = read(pipe_fds[0], buffer, sizeof buffer)) > 0;) text.append(buffer, std::size_t(n));
    close(pipe_fds[0]);
    waitpid(pid, &status, 0);
    return text;
}

std::string run(Order order, int& status) { return run_child([order] { fault(order); }, status); }

int failures = 0;

void expect(bool condition, const char* what, const std::string& report) {
    if (condition) return;
    std::printf("FAIL: %s\n%s\n", what, report.c_str());
    ++failures;
}

}

int main() {
    for (Order order : {Order::report_only, Order::nested_raise, Order::nested_raise_siginfo, Order::raise_after_return}) {
        int status = 0;
        const std::string report = run(order, status);
        expect(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "the process ends with the signal", report);
        expect(report.find(order == Order::raise_after_return
                               ? "crash_test: fatal signal 11 sent by process "
                               : "crash_test: fatal signal 11 at 0x1238,") != std::string::npos,
               "the fault address, or the sender when the fault's frame is gone", report);
        expect(order != Order::raise_after_return || report.find(", last fault at 0x1238,") != std::string::npos,
               "the thread's last fault address", report);
        expect(report.find("  x0=0x") != std::string::npos && report.find(" sp=0x") != std::string::npos,
               "the registers", report);
        const bool nested = order == Order::nested_raise || order == Order::nested_raise_siginfo;
        expect((report.find("raised again") != std::string::npos) == nested,
               "a raise from inside another handler is named", report);
        const auto pc = report.find("  pc ");
        expect(pc != std::string::npos && report.find("5fault", pc) < report.find('\n', pc),
               "pc names the faulting function", report);
        expect(report.find("port details") != std::string::npos, "the port's details", report);
    }
    for (bool overflowing : {false, true}) {
        int status = 0;
        const std::string report = run_child([overflowing] { fault_guest(overflowing); }, status);
        expect(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "a guest thread's fault ends the process with the signal", report);
        expect(report.find("crash_test: fatal signal 11 at 0x") != std::string::npos, "a guest thread's fault is reported", report);
        expect(report.find("  on guest thread guest16, OSThread 0x") != std::string::npos, "the report names the guest thread", report);
        const auto pc = report.find("  pc ");
        const char* function = overflowing ? "8overflow" : "14fault_on_guest";
        expect(pc != std::string::npos && report.find(function, pc) < report.find('\n', pc), "pc names the guest's function", report);
        expect(report.find("  at ") != std::string::npos, "a backtrace on the guest stack", report);
    }
#if defined(__aarch64__)
    {
        int status = 0;
        const std::string report = run_child([] { abort_in_async_handler(); }, status);
        if (report.find("asynchronous interrupts stay off") == std::string::npos) {
            expect(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "an abort in an asynchronous interrupt ends the process", report);
            expect(report.find("crash_test: fatal signal 6 sent by process ") != std::string::npos, "the abort is reported", report);
            expect(report.find("raised again") == std::string::npos, "the interrupt's signal frame is not taken for a fault", report);
            expect(report.find("guest16, OSThread 0x") != std::string::npos && report.find(", in an interrupt handler") != std::string::npos,
                   "the report names the interrupted thread and the handler", report);
        }
    }
#endif
    std::printf("%s\n", failures ? "crash_report: failures" : "crash_report: ok");
    return failures ? 1 : 0;
}
