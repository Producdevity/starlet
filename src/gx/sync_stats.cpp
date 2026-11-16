#include "starlet/gx/sync_stats.hpp"

#include <pthread.h>
#include <time.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace starlet::gx::sync_stats {
namespace {
using clock = std::chrono::steady_clock;
long long now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count(); }
struct Entry { unsigned long long count = 0; double ms = 0, max_ms = 0; };
struct State {
    std::mutex mutex;
    std::map<std::pair<std::string, std::string>, Entry> entries;
    std::map<std::string, unsigned long long> counters;
    std::map<std::string, std::string> states;
    long long last = 0;
    bool on = [] { const char* v = std::getenv("STARLET_GX_SYNC_STATS"); return v && v[0] == '1'; }();
};
State& st() { static State s; return s; }
thread_local const char* current_context = nullptr;

void report(State& s, long long now) {
    std::fprintf(stderr, "syncstats: window %.1f s", double(now - s.last) / 1e9);
    for (const auto& c : s.counters) std::fprintf(stderr, ", %s %llu", c.first.c_str(), c.second);
    std::fprintf(stderr, "\n");
    for (const auto& t : s.states) std::fprintf(stderr, "syncstats: state %s\n", t.second.c_str());
    for (const auto& e : s.entries)
        std::fprintf(stderr, "syncstats: %-34s %-14s n=%llu total_ms=%.1f max_ms=%.1f\n", e.first.first.c_str(), e.first.second.c_str(),
                     e.second.count, e.second.ms, e.second.max_ms);
    std::fflush(stderr);
    s.entries.clear();
    s.counters.clear();
    s.last = now;
}
} // namespace

bool enabled() { return st().on; }

Context::Context(const char* cause) : previous(current_context) { if (st().on) current_context = cause; }
Context::~Context() { if (st().on) current_context = previous; }
const char* context(const char* fallback) { return current_context ? current_context : fallback; }

Wait::Wait(const char* c) : cause(c), start(st().on ? now_ns() : 0) {}
Wait::~Wait() { if (start) note(cause, double(now_ns() - start) / 1e6); }

namespace {
long long cpu_ns() {
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (long long)t.tv_sec * 1000000000ll + t.tv_nsec;
}
}
Task::Task(const char* t) : tag(t), start(st().on ? now_ns() : 0), cpu(st().on ? cpu_ns() : 0) {}
Task::~Task() {
    if (!start) return;
    const double wall = double(now_ns() - start) / 1e6, used = double(cpu_ns() - cpu) / 1e6;
    note(tag, wall);
    std::string blocked = std::string(tag) + " blocked";
    auto& s = st();
    char name[32] = "?";
    pthread_getname_np(pthread_self(), name, sizeof name);
    std::lock_guard<std::mutex> lock(s.mutex);
    auto& e = s.entries[{blocked, name}];
    const double ms = wall > used ? wall - used : 0;
    ++e.count;
    e.ms += ms;
    if (ms > e.max_ms) e.max_ms = ms;
}

void presented() {
    auto& s = st();
    if (!s.on) return;
    static long long last = 0;
    const long long now = now_ns();
    if (last) {
        const double ms = double(now - last) / 1e6;
        const char* bucket = ms < 25 ? "present interval <=1 retrace" : ms < 41.7 ? "present interval 2 retraces"
                             : ms < 58.4 ? "present interval 3 retraces" : ms < 75 ? "present interval 4 retraces"
                             : "present interval 5+ retraces";
        note(bucket, ms);
    }
    last = now;
}

void note(const char* cause, double ms) {
    auto& s = st();
    if (!s.on) return;
    char name[32] = "?";
    pthread_getname_np(pthread_self(), name, sizeof name);
    std::lock_guard<std::mutex> lock(s.mutex);
    auto& e = s.entries[{cause, name}];
    ++e.count;
    e.ms += ms;
    if (ms > e.max_ms) e.max_ms = ms;
}

void tick(const char* counter) {
    auto& s = st();
    if (!s.on) return;
    std::lock_guard<std::mutex> lock(s.mutex);
    const long long now = now_ns();
    if (!s.last) s.last = now;
    ++s.counters[counter];
    if (now - s.last >= 10000000000ll) report(s, now);
}

void state(const std::string& text) {
    auto& s = st();
    if (!s.on) return;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.states[text.substr(0, text.find('='))] = text;
}

} // namespace starlet::gx::sync_stats
