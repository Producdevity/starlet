// Samples come from SIGPROF: a process CPU-time timer, or a sampler thread signalling each thread for
// wall-clock profiles. Frames are read with process_vm_readv, so a frame chain through code without
// frame pointers ends instead of faulting.
#include "starlet/os/profiler.hpp"

#include "starlet/os/thread_name.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace starlet::os {
namespace {

constexpr int frame_depth = 6;

struct Sample {
    std::uint32_t milliseconds;
    std::uint32_t thread;
    std::uintptr_t pc;
    std::uintptr_t lr;
    std::uintptr_t frames[frame_depth];
};

constexpr std::size_t capacity = std::size_t{1} << 18;
Sample* samples = nullptr;
std::atomic<std::size_t> taken{0};
timespec started{};
std::uint32_t from_ms = 0;
std::vector<std::uint32_t> frames;
std::atomic<std::size_t> frame_count{0};

std::uint32_t elapsed_ms() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return std::uint32_t((now.tv_sec - started.tv_sec) * 1000 + (now.tv_nsec - started.tv_nsec) / 1000000);
}

#if defined(__linux__)
void on_sample(int, siginfo_t*, void* context) {
    const std::uint32_t milliseconds = elapsed_ms();
    if (milliseconds < from_ms) return;
    const std::size_t index = taken.fetch_add(1, std::memory_order_relaxed);
    if (index >= capacity) return;
    Sample& s = samples[index];
    s.milliseconds = milliseconds;
    s.thread = std::uint32_t(syscall(SYS_gettid));
#if defined(__aarch64__)
    const auto& registers = static_cast<ucontext_t*>(context)->uc_mcontext;
    s.pc = registers.pc;
    s.lr = registers.regs[30];
    // AArch64 frame record: [fp] = caller's fp, [fp + 8] = return address.
    std::uintptr_t fp = registers.regs[29];
    const std::uintptr_t sp = registers.sp;
    const pid_t pid = getpid();
    for (int i = 0; i < frame_depth; ++i) {
        s.frames[i] = 0;
        if (fp < sp || fp - sp > (std::uintptr_t{8} << 20) || (fp & 7)) break;
        std::uintptr_t record[2];
        iovec local{record, sizeof record}, remote{reinterpret_cast<void*>(fp), sizeof record};
        if (process_vm_readv(pid, &local, 1, &remote, 1, 0) != ssize_t(sizeof record)) break;
        s.frames[i] = record[1];
        if (record[0] <= fp) break;
        fp = record[0];
    }
#else
    (void)context;
    s.pc = s.lr = 0;
#endif
}
#endif

bool frame_stats = false;

const char* setting(const std::string& prefix, const char* name) {
    return std::getenv((prefix + "_" + name).c_str());
}

}

void start_profiler(const char* prefix_text) {
    const std::string prefix = prefix_text;
    frame_stats = setting(prefix, "FRAME_STATS") != nullptr;
#if defined(__linux__)
    const char* rate = setting(prefix, "PROFILE");
    if (!rate || samples) return;
    const int hz = std::atoi(rate) > 0 ? std::atoi(rate) : 1000;
    samples = static_cast<Sample*>(std::calloc(capacity, sizeof(Sample)));
    if (!samples) return;
    frames.resize(1 << 16);
    clock_gettime(CLOCK_MONOTONIC, &started);
    if (const char* from = setting(prefix, "PROFILE_FROM")) from_ms = std::uint32_t(std::atoi(from)) * 1000u;
    struct sigaction action {};
    action.sa_sigaction = on_sample;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPROF, &action, nullptr);
    if (setting(prefix, "PROFILE_WALL")) {
        // Wall clock: every thread, running or blocked, is sampled at the rate.
        std::vector<std::string> prefixes;
        if (const char* list = setting(prefix, "PROFILE_THREADS")) {
            std::string all = list;
            for (std::size_t at = 0; at <= all.size();) {
                const std::size_t end = std::min(all.find(',', at), all.size());
                if (end > at) prefixes.push_back(all.substr(at, end - at));
                at = end + 1;
            }
        }
        std::thread([hz, prefixes] {
            name_thread("profiler");
            const pid_t pid = getpid(), me = pid_t(syscall(SYS_gettid));
            char process[64] = {};
            if (std::FILE* f = std::fopen("/proc/self/comm", "r")) {
                if (!std::fgets(process, sizeof process, f)) process[0] = 0;
                std::fclose(f);
            }
            std::unordered_map<pid_t, bool> chosen;
            auto wanted = [&](pid_t tid) {
                if (prefixes.empty()) return true;
                const auto found = chosen.find(tid);
                if (found != chosen.end()) return found->second;
                char name[64] = {};
                const std::string comm = "/proc/self/task/" + std::to_string(tid) + "/comm";
                if (std::FILE* f = std::fopen(comm.c_str(), "r")) {
                    if (!std::fgets(name, sizeof name, f)) name[0] = 0;
                    std::fclose(f);
                }
                bool match = false;
                for (const auto& prefix : prefixes) match = match || std::strncmp(name, prefix.c_str(), prefix.size()) == 0;
                // Threads are named after they start; decide once the name is set.
                if (std::strcmp(name, process) != 0) chosen[tid] = match;
                return match;
            };
            for (;;) {
                usleep(useconds_t(1000000 / hz));
                if (DIR* tasks = opendir("/proc/self/task")) {
                    while (dirent* entry = readdir(tasks)) {
                        const pid_t tid = pid_t(std::atoi(entry->d_name));
                        if (tid > 0 && tid != me && wanted(tid)) syscall(SYS_tgkill, pid, tid, SIGPROF);
                    }
                    closedir(tasks);
                }
            }
        }).detach();
        std::printf("Profiler: %d Hz of wall-clock time per thread\n", hz);
        return;
    }
    itimerval timer{};
    timer.it_interval.tv_usec = 1000000 / hz;
    timer.it_value = timer.it_interval;
    setitimer(ITIMER_PROF, &timer, nullptr);
    std::printf("Profiler: %d Hz of CPU time\n", hz);
#endif
}

void write_profile(const char* capture_dir) {
#if defined(__linux__)
    if (!samples) return;
    itimerval off{};
    setitimer(ITIMER_PROF, &off, nullptr);
    const std::string path = std::string(capture_dir ? capture_dir : "/tmp") + "/profile.txt";
    std::FILE* out = std::fopen(path.c_str(), "w");
    if (!out) return;
    if (DIR* tasks = opendir("/proc/self/task")) {
        while (dirent* entry = readdir(tasks)) {
            if (entry->d_name[0] == '.') continue;
            char name[64] = {};
            const std::string comm = std::string("/proc/self/task/") + entry->d_name + "/comm";
            if (std::FILE* f = std::fopen(comm.c_str(), "r")) {
                if (std::fgets(name, sizeof name, f))
                    for (char* c = name; *c; ++c)
                        if (*c == '\n') *c = 0;
                std::fclose(f);
            }
            std::fprintf(out, "# thread %s %s\n", entry->d_name, name);
        }
        closedir(tasks);
    }
    if (std::FILE* maps = std::fopen("/proc/self/maps", "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, maps))
            if (std::strstr(line, "r-xp")) std::fprintf(out, "# map %s", line);
        std::fclose(maps);
    }
    const std::size_t marked = std::min(frame_count.load(), frames.size());
    for (std::size_t i = 0; i < marked; ++i) std::fprintf(out, "# frame %u\n", frames[i]);
    const std::size_t count = std::min(taken.load(), capacity);
    for (std::size_t i = 0; i < count; ++i) {
        const Sample& sample = samples[i];
        std::fprintf(out, "%u %u %lx %lx", sample.milliseconds, sample.thread, (unsigned long)sample.pc, (unsigned long)sample.lr);
        for (int f = 0; f < frame_depth && sample.frames[f]; ++f) std::fprintf(out, " %lx", (unsigned long)sample.frames[f]);
        std::fputc('\n', out);
    }
    std::fclose(out);
    std::printf("Profiler: %zu samples in %s\n", count, path.c_str());
#endif
}

void profile_frame() {
    const bool stats = frame_stats;
    if (!samples && !stats) return;
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (samples) {
        const std::uint32_t ms = elapsed_ms();
        if (ms >= from_ms) {
            const std::size_t i = frame_count.fetch_add(1, std::memory_order_relaxed);
            if (i < frames.size()) frames[i] = ms;
        }
    }
    if (!stats) return;
    // Intervals between presentations, in buckets of the 59.94 Hz retrace.
    static timespec last{}, window{};
    static unsigned buckets[4] = {}, count = 0;
    static double longest = 0;
    const auto ms = [](const timespec& a, const timespec& b) { return (a.tv_sec - b.tv_sec) * 1000.0 + (a.tv_nsec - b.tv_nsec) / 1e6; };
    if (last.tv_sec) {
        const double interval = ms(now, last);
        ++buckets[interval < 25 ? 0 : interval < 42 ? 1 : interval < 58 ? 2 : 3];
        longest = std::max(longest, interval);
        ++count;
    } else {
        window = now;
    }
    last = now;
    if (ms(now, window) >= 10000) {
        std::printf("frames: %u in %.1f s; intervals <25 ms %u, 25-42 %u, 42-58 %u, >=58 %u; longest %.1f ms\n", count,
                    ms(now, window) / 1000, buckets[0], buckets[1], buckets[2], buckets[3], longest);
        window = now;
        std::fill(buckets, buckets + 4, 0u);
        count = 0;
        longest = 0;
    }
}

}
