// The fatal signal report describes the original fault when another handler caught it first and
// raised the signal again: SDL's console cleanup restores the previous handler, unblocks the signal
// and raises it from inside its own handler; a plain handler's raise arrives after it returns.
#include "starlet/os/crash_report.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

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

[[noreturn]] void fault(Order order) {
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

std::string run(Order order, int& status) {
    int pipe_fds[2];
    if (pipe(pipe_fds)) return {};
    const pid_t child = fork();
    if (child == 0) {
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[0]);
        fault(order);
    }
    close(pipe_fds[1]);
    std::string text;
    char buffer[4096];
    for (ssize_t n; (n = read(pipe_fds[0], buffer, sizeof buffer)) > 0;) text.append(buffer, std::size_t(n));
    close(pipe_fds[0]);
    waitpid(child, &status, 0);
    return text;
}

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
    std::printf("%s\n", failures ? "crash_report: failures" : "crash_report: ok");
    return failures ? 1 : 0;
}
