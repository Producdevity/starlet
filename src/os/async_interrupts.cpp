// Asynchronous interrupts (enable_async_interrupts): a watchdog thread signals the guest CPU thread
// while interrupts stay pending and it keeps running; the handler takes them when the interrupted
// code and every frame below it down to the thread's entry are registered guest code.
#include "async_interrupts.hpp"

#include "fiber.hpp"
#include "starlet/os/crash_report.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/os/host_allocation.hpp"
#include "starlet/os/thread_name.hpp"

#include "starlet/sdk/os.h"

#include <dlfcn.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define STARLET_ASYNC_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define STARLET_ASYNC_TSAN 1
#endif
// TSan runs signal handlers later, at its own interception points, with the context of the
// original interruption: the frame check would vouch for code that is no longer running.
#if defined(__aarch64__) && defined(__linux__) && !defined(STARLET_ASYNC_TSAN)
#define STARLET_ASYNC_INTERRUPTS 1
#endif

namespace starlet::os {
namespace {

constexpr int max_ranges = 16;
std::atomic<std::uintptr_t> range_begin[max_ranges], range_end[max_ranges];
std::atomic<int> range_count{0};
std::mutex registering;

// A log line, written with one write so other threads' output does not split it.
class Line {
public:
    Line& text(const char* text) {
        while (*text && used_ + 1 < sizeof buffer_) buffer_[used_++] = *text++;
        return *this;
    }
    Line& hex(std::uintptr_t value) {
        char digits[17];
        int n = 16;
        digits[n] = 0;
        do digits[--n] = "0123456789abcdef"[value & 15];
        while (value >>= 4);
        return text("0x").text(digits + n);
    }
    // module+0xoffset, as crash reports name code addresses.
    Line& location(std::uintptr_t address) {
        Dl_info module{};
        if (!dladdr(reinterpret_cast<void*>(address), &module) || !module.dli_fname) return text("unknown module");
        const char* file = std::strrchr(module.dli_fname, '/');
        return text(file ? file + 1 : module.dli_fname).text("+").hex(address - reinterpret_cast<std::uintptr_t>(module.dli_fbase));
    }
    void write() {
        buffer_[used_++] = '\n';
        (void)!::write(STDERR_FILENO, buffer_, used_);
    }

private:
    char buffer_[256];
    std::size_t used_ = 0;
};

#if STARLET_ASYNC_INTERRUPTS

bool in_guest_code(std::uintptr_t address) {
    const int count = range_count.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i)
        if (address >= range_begin[i].load(std::memory_order_relaxed) && address < range_end[i].load(std::memory_order_relaxed))
            return true;
    return false;
}

constexpr std::uint64_t stall_report_ns = 1000000000u;
constexpr unsigned retry_ms = 2;
std::atomic<unsigned> threshold_ms{20};

struct Interrupted {
    std::uintptr_t pc, lr, fp, sp;
};

// Why the interrupted code is no safe point, or null when it is one. Guest code keeps frame
// records, so a host function on the stack below guest code shows as a return address: in the
// record of the guest function it called, or in the link register while that function has no
// record (a leaf, a prologue or an epilogue).
const char* unsafe_frames(const Interrupted& at, const async::Interruptee& g) {
    const std::uintptr_t entry_return = fiber::entry_return_address();
    if (!in_guest_code(at.pc)) return "running host code";
    if (at.lr != entry_return && !in_guest_code(at.lr)) return "a host frame on the stack";
    if (at.sp < g.stack_low || at.sp >= g.stack_high) return "off its stack";
    for (std::uintptr_t fp = at.fp; fp != g.entry_frame;) {
        if (fp < at.sp || fp > g.entry_frame || (fp & 7)) return "a broken frame chain";
        const auto* record = reinterpret_cast<const std::uintptr_t*>(fp);
        const std::uintptr_t next = record[0], ret = record[1];
        if (next <= fp) return "a broken frame chain";
        if (ret == entry_return ? next != g.entry_frame : !in_guest_code(ret)) return "a host frame on the stack";
        fp = next;
    }
    return nullptr;
}

// The last refused attempt, for the watchdog's report: written by the signal handler, read by the
// watchdog under a sequence count.
constexpr int stall_frames = 16;
struct Stall {
    std::atomic<std::uint32_t> sequence;
    std::atomic<std::uint64_t> time;
    std::atomic<const char*> reason;
    std::atomic<std::uint64_t> name[2];
    std::atomic<std::uintptr_t> words[2 + stall_frames];  // pc, lr, then return addresses.
    std::atomic<int> count;
};
Stall stall;

struct StallCopy {
    std::uint64_t time;
    const char* reason;
    char name[17];
    std::uintptr_t words[2 + stall_frames];
    int count;
};

void keep_stall(const Interrupted& at, const async::Interruptee& g, const char* reason) {
    const std::uint32_t sequence = stall.sequence.load(std::memory_order_relaxed);
    stall.sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    stall.time.store(async::now_ns(), std::memory_order_relaxed);
    stall.reason.store(reason, std::memory_order_relaxed);
    std::uint64_t name[2] = {};
    std::memcpy(name, g.name, strnlen(g.name, sizeof name));
    stall.name[0].store(name[0], std::memory_order_relaxed);
    stall.name[1].store(name[1], std::memory_order_relaxed);
    int count = 0;
    stall.words[count++].store(at.pc, std::memory_order_relaxed);
    stall.words[count++].store(at.lr, std::memory_order_relaxed);
    for (std::uintptr_t fp = at.fp; count < 2 + stall_frames && fp != g.entry_frame;) {
        if (fp < at.sp || fp >= g.stack_high || (fp & 7)) break;
        const auto* record = reinterpret_cast<const std::uintptr_t*>(fp);
        stall.words[count++].store(record[1], std::memory_order_relaxed);
        if (record[0] <= fp) break;
        fp = record[0];
    }
    stall.count.store(count, std::memory_order_relaxed);
    stall.sequence.store(sequence + 2, std::memory_order_release);
}

bool read_stall(StallCopy& out) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::uint32_t sequence = stall.sequence.load(std::memory_order_acquire);
        if (!sequence || (sequence & 1)) continue;
        out.time = stall.time.load(std::memory_order_relaxed);
        out.reason = stall.reason.load(std::memory_order_relaxed);
        const std::uint64_t name[2] = {stall.name[0].load(std::memory_order_relaxed), stall.name[1].load(std::memory_order_relaxed)};
        std::memcpy(out.name, name, sizeof name);
        out.name[16] = 0;
        out.count = std::min(stall.count.load(std::memory_order_relaxed), 2 + stall_frames);
        for (int i = 0; i < out.count; ++i) out.words[i] = stall.words[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (stall.sequence.load(std::memory_order_relaxed) == sequence) return true;
    }
    return false;
}

// Program counters already logged, by the signal handler on the guest CPU thread only.
std::uintptr_t taken_at[64];
int taken_count = 0;

void note_taken(std::uintptr_t pc, const char* name) {
    for (int i = 0; i < taken_count; ++i)
        if (taken_at[i] == pc) return;
    if (taken_count == int(sizeof taken_at / sizeof taken_at[0])) return;
    taken_at[taken_count++] = pc;
    Line().text("[os] took interrupts asynchronously in guest thread ").text(name).text(" at PC ").hex(pc).text(" (")
        .location(pc).text(")").write();
}

void on_signal(int, siginfo_t*, void* context) {
    const int saved_errno = errno;
    const auto& registers = static_cast<const ucontext_t*>(context)->uc_mcontext;
    const Interrupted at{registers.pc, registers.regs[30], registers.regs[29], registers.sp};
    const async::Interruptee g = async::interruptee();
    const char* refusal = g.refusal ? g.refusal : unsafe_frames(at, g);
    // Another source's SIGURG, or no interrupt left to take.
    if (!g.name || refusal == async::nothing_pending) {
    } else if (refusal) {
        keep_stall(at, g, refusal);
    } else {
        // Only guest code is on this stack: no host lock or host state is held, as at a checkpoint.
        note_taken(at.pc, g.name);
        // The kernel blocks the signal until this handler returns, which a hand-over to another
        // guest thread postpones; sigreturn restores the interrupted code's mask.
        sigset_t own;
        sigemptyset(&own);
        sigaddset(&own, async::interrupt_signal);
        pthread_sigmask(SIG_UNBLOCK, &own, nullptr);
        async::take_interrupts();
    }
    errno = saved_errno;
}

// One report per stall, and per program counter: the stack where the last attempt found no safe point.
bool report_stall(std::uint64_t since) {
    StallCopy copy;
    if (!read_stall(copy) || copy.time < since) return false;
    static std::uintptr_t reported[16];
    static int reported_count = 0;
    for (int i = 0; i < reported_count; ++i)
        if (reported[i] == copy.words[0]) return true;
    if (reported_count == int(sizeof reported / sizeof reported[0])) return true;
    reported[reported_count++] = copy.words[0];
    Line().text("[os] no safe point in 1 s to take pending interrupts in guest thread ").text(copy.name)
        .text(": ").text(copy.reason).write();
    report_frames("  pc ", &copy.words[0], 1);
    report_frames("  lr ", &copy.words[1], 1);
    report_frames("  frame ", &copy.words[2], std::size_t(copy.count - 2));
    return true;
}

// The CPU time of another thread of this process: the kernel's clock id for it.
bool thread_cpu_ns(pid_t thread, std::uint64_t& ns) {
    const clockid_t clock = clockid_t(int(~unsigned(thread) << 3) | 6);  // CPUCLOCK_PERTHREAD | CPUCLOCK_SCHED
    timespec t{};
    if (clock_gettime(clock, &t) != 0) return false;
    ns = std::uint64_t(t.tv_sec) * 1000000000u + std::uint64_t(t.tv_nsec);
    return true;
}

// Signals the guest CPU thread while interrupts have been pending past the threshold and it used at
// least half of the time since the last look (a thread blocked in a system call is not signalled).
[[noreturn]] void watch() {
    name_thread("irq-watchdog");
    pid_t watched = 0;
    std::uint64_t last_wall = 0, last_cpu = 0, episode = 0;
    bool reported = false;
    unsigned period = 1;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(period));
        const unsigned threshold = threshold_ms.load(std::memory_order_relaxed);
        period = std::max(1u, threshold / 2);
        const pid_t thread = async::guest_cpu_thread();
        const std::uint64_t now = async::now_ns();
        std::uint64_t cpu = 0;
        const bool measured = thread && thread_cpu_ns(thread, cpu);
        const bool busy = measured && thread == watched && (cpu - last_cpu) * 2 >= now - last_wall;
        watched = measured ? thread : 0;
        last_wall = now;
        last_cpu = cpu;
        const std::uint64_t since = async::pending_since();
        if (!watched || !since || now < since + std::uint64_t(threshold) * 1000000u) continue;
        period = retry_ms;
        if (since != episode) {
            episode = since;
            reported = false;
        }
        if (busy) syscall(SYS_tgkill, getpid(), thread, async::interrupt_signal);
        if (!reported && now >= since + stall_report_ns) reported = report_stall(since);
    }
}

#endif

}

bool add_guest_code(const void* begin, const void* end) {
    const auto first = reinterpret_cast<std::uintptr_t>(begin), last = reinterpret_cast<std::uintptr_t>(end);
    // Registering the whole executable would take Starlet's own code for the game's.
    const std::uintptr_t starlet_code[] = {
        reinterpret_cast<std::uintptr_t>(&checkpoint),
        reinterpret_cast<std::uintptr_t>(&raise_interrupt),
        reinterpret_cast<std::uintptr_t>(&add_guest_code),
        reinterpret_cast<std::uintptr_t>(&OSSleepThread),
        reinterpret_cast<std::uintptr_t>(&report_text),
        fiber::entry_return_address(),
    };
    const char* refusal = first >= last ? "is empty" : nullptr;
    for (const std::uintptr_t code : starlet_code)
        if (code >= first && code < last) refusal = "holds Starlet's own code";
    host_allocation_scope host_;
    std::lock_guard<std::mutex> lock(registering);
    const int count = range_count.load(std::memory_order_relaxed);
    if (!refusal && count == max_ranges) refusal = "is one range too many";
    if (refusal) {
        Line().text("[os] guest code ").hex(first).text("-").hex(last).text(" ").text(refusal).text("; not registered").write();
        return false;
    }
    range_begin[count].store(first, std::memory_order_relaxed);
    range_end[count].store(last, std::memory_order_relaxed);
    range_count.store(count + 1, std::memory_order_release);
    return true;
}

bool enable_async_interrupts(unsigned threshold) {
#if STARLET_ASYNC_INTERRUPTS
    threshold_ms.store(std::max(1u, threshold), std::memory_order_relaxed);
    host_allocation_scope host_;
    std::lock_guard<std::mutex> lock(registering);
    static bool enabled = false;
    if (enabled) return true;
    struct sigaction action {};
    action.sa_sigaction = on_signal;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(async::interrupt_signal, &action, nullptr) != 0) {
        Line().text("[os] asynchronous interrupts stay off: no signal handler").write();
        return false;
    }
    async::track_pending();
    std::thread(watch).detach();
    enabled = true;
    return true;
#else
    (void)threshold;
    Line().text("[os] asynchronous interrupts stay off: they need AArch64 Linux without TSan").write();
    return false;
#endif
}

}
