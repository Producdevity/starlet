#include "starlet/os/guest_cpu.hpp"

#include "async_interrupts.hpp"
#include "fiber.hpp"
#include "starlet/os/crash_report.hpp"
#include "starlet/os/game_memory.hpp"
#include "starlet/os/host_allocation.hpp"
#include "starlet/os/thread_name.hpp"

#include "starlet/sdk/os.h"

#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

using namespace starlet::sdk;

namespace starlet::os {
namespace {

constexpr int interrupt_priority = -1;
constexpr int priority_slots = OS_PRIORITY_MAX + 2;  // Slot 0 is the interrupt thread.
// Native frames are larger than the console's, and host library calls (stdio,
// SDL, GL) run on guest stacks, so stacks get at least this much.
constexpr std::size_t minimum_stack = std::size_t{1} << 20;

struct Guest {
    OSThread* os = nullptr;
    fiber::Context fiber;
    bool interrupts = true;  // MSR[EE] of this thread's context.
    bool interrupt_thread = false;
    bool queued = false;      // In a ready slot.
    bool killed = false;      // Cancelled while it held the CPU: it ends at its next switch.
    bool detached = false;    // Ended detached: the context that frees its stack deletes it.
    void* stack = nullptr;
    std::size_t stack_size = 0;
    void* (*entry)(void*) = nullptr;
    int (*main)() = nullptr;  // The entry of run_main_thread's thread, instead of entry.
    void* parameter = nullptr;
    void* entry_frame = nullptr;  // fiber::call_entry's frame record on this thread's stack.
    char name[16] = {};
    int priority() const { return interrupt_thread ? interrupt_priority : os->priority; }
};

std::mutex scheduler;
Guest* running = nullptr;
std::array<std::deque<Guest*>, priority_slots> ready;
std::atomic<int> best_ready{priority_slots};  // Slot index of the best ready thread.
std::deque<std::function<void()>> pending;
std::condition_variable interrupt_raised;
std::atomic<bool> pending_flag{false};
std::atomic<bool> tracking_pending{false};
std::atomic<std::uint64_t> pending_since_ns{0};  // CLOCK_MONOTONIC; 0 while none are pending.
std::unordered_map<OSThread*, Guest*> guests;
int scheduler_disabled = 0;
Guest interrupt_guest;
OSThread* last_guest = nullptr;
std::atomic<SwitchTrace> switch_trace{nullptr};

// The guest CPU's host thread: its own context runs only while no guest thread is ready.
fiber::Context idle;
std::atomic<pid_t> cpu_thread{0};
std::condition_variable cpu_wake;
bool cpu_waiting = false;
bool cpu_stopped = false;  // The main thread ended.
Guest* main_guest = nullptr;
int main_result = 0;
// A thread that ended and switched away from its stack, which the next context frees.
Guest* exited = nullptr;

// The guest thread running on this host thread (only the guest CPU's thread has one).
thread_local Guest* self = nullptr;
thread_local bool handling_in_place = false;
std::uint64_t run_cpu_start = 0;

std::uint64_t thread_cpu_us() {
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return std::uint64_t(t.tv_sec) * 1000000u + std::uint64_t(t.tv_nsec) / 1000u;
}

std::atomic<bool> in_handler{false};

std::uint64_t monotonic_ns() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return std::uint64_t(t.tv_sec) * 1000000000u + std::uint64_t(t.tv_nsec);
}

// Takes the next pending handler, with the scheduler locked.
std::function<void()> next_handler() {
    auto handler = std::move(pending.front());
    pending.pop_front();
    const bool more = !pending.empty();
    pending_flag.store(more, std::memory_order_release);
    if (!more) pending_since_ns.store(0, std::memory_order_relaxed);
    return handler;
}

int slot(const Guest* g) { return g->priority() + 1; }

// Diagnostics (set_switch_trace): h handed the CPU, g resumed, x released with its CPU time.
void trace_dispatch(const Guest* g) {
    if (const auto trace = switch_trace.load(std::memory_order_relaxed)) trace('h', std::uint32_t(slot(g)));
}
void trace_resume(const Guest* g) {
    const auto trace = switch_trace.load(std::memory_order_relaxed);
    if (!trace) return;
    run_cpu_start = thread_cpu_us();
    trace('g', std::uint32_t(slot(g)));
}
void trace_release(const Guest* g) {
    const auto trace = switch_trace.load(std::memory_order_relaxed);
    if (!trace) return;
    const std::uint64_t used = run_cpu_start ? thread_cpu_us() - run_cpu_start : 0;
    trace('x', std::uint32_t(slot(g)) | (g->queued ? 0x80u : 0u) | std::uint32_t(std::min<std::uint64_t>(used, 0xffffff) << 8));
}

void update_best() {
    int best = priority_slots;
    for (int i = 0; i < priority_slots; ++i)
        if (!ready[i].empty()) { best = i; break; }
    best_ready.store(best, std::memory_order_release);
}

void enqueue(Guest* g, bool front = false) {
    if (g->queued) return;
    if (front) ready[slot(g)].push_front(g);
    else ready[slot(g)].push_back(g);
    g->queued = true;
    if (!g->interrupt_thread) g->os->state = OS_THREAD_STATE_READY;
    update_best();
}

void dequeue(Guest* g) {
    if (!g->queued) return;
    auto& q = ready[slot(g)];
    q.erase(std::find(q.begin(), q.end(), g));
    g->queued = false;
    update_best();
}

Guest* take_next() {
    if (running) throw std::logic_error("dispatch with the CPU held");
    for (int i = 0; i < priority_slots; ++i) {
        if (ready[i].empty()) continue;
        Guest* next = ready[i].front();
        ready[i].pop_front();
        next->queued = false;
        update_best();
        running = next;
        trace_dispatch(next);
        if (!next->interrupt_thread) {
            next->os->state = OS_THREAD_STATE_RUNNING;
            last_guest = next->os;
        }
        return next;
    }
    return nullptr;
}

// Host threads (interrupts, wakeups) that find the CPU idle wake its host thread.
void wake_cpu() {
    if (!running && cpu_waiting) cpu_wake.notify_one();
}

void release_fiber(Guest* g) {
    fiber::release(g->fiber);
    if (g->stack) release_stack(g->stack, g->stack_size);
    g->stack = nullptr;
}

// Each context does this when it resumes, under the scheduler lock: the thread that ended and
// switched here is off its stack now.
void free_exited() {
    Guest* g = exited;
    if (!g) return;
    exited = nullptr;
    release_fiber(g);
    if (g->detached) delete g;
}

// Continues on to (the CPU thread's own context when null) and returns once this context is
// switched to again. The lock is not held across the switch: a host thread may raise an interrupt
// in between, which only queues it.
void transfer(fiber::Context& from, Guest* to, std::unique_lock<std::mutex>& lock) {
    lock.unlock();
    self = to;
    fiber::switch_to(from, to ? to->fiber : idle);
    lock.lock();
    free_exited();
}

// The running thread g ended: its stack is freed by the next context.
[[noreturn]] void end_current(Guest* g, std::unique_lock<std::mutex>& lock) {
    trace_release(g);
    running = nullptr;
    g->detached = g->os->attr & OS_THREAD_ATTR_DETACH;
    if (exited) throw std::logic_error("two guest threads ended without a switch between them");
    exited = g;
    Guest* next = nullptr;
    if (g == main_guest) cpu_stopped = true;
    else next = take_next();
    transfer(g->fiber, next, lock);
    std::abort();
}

// Gives up the CPU (the caller has already queued or parked itself) and returns once this
// thread is dispatched again.
void switch_away(std::unique_lock<std::mutex>& lock) {
    Guest* g = self;
    if (g->killed) end_current(g, lock);
    trace_release(g);
    running = nullptr;
    Guest* next = take_next();
    if (next != g) transfer(g->fiber, next, lock);
    trace_resume(g);
}

// Runs the pending interrupt handlers on the running thread, as the console's CPU takes an
// interrupt on the thread it interrupts, instead of handing the CPU to the interrupt thread and back.
void handle_in_place(Guest* g, std::unique_lock<std::mutex>& lock) {
    handling_in_place = true;
    in_handler.store(true, std::memory_order_relaxed);
    const bool enabled = g->interrupts;
    while (!pending.empty()) {
        auto handler = next_handler();
        g->interrupts = false;
        lock.unlock();
        {
            host_allocation_scope host_;
            handler();
        }
        lock.lock();
    }
    dequeue(&interrupt_guest);
    g->interrupts = enabled;
    in_handler.store(false, std::memory_order_relaxed);
    handling_in_place = false;
}

// Takes pending interrupts while this thread has interrupts enabled, then lets a
// better thread run. A yield also gives way to equal priority.
void reschedule(std::unique_lock<std::mutex>& lock, bool yield) {
    Guest* g = self;
    if (!g || running != g || g->interrupt_thread || handling_in_place) return;
    if (scheduler_disabled && !yield) return;
    const int mine = slot(g);
    const auto best_slot = [g] {
        for (int i = g->interrupts ? 0 : 1; i < priority_slots; ++i)
            if (!ready[i].empty()) return i;
        return priority_slots;
    };
    int best = best_slot();
    if (best == 0) {
        if (const auto trace = switch_trace.load(std::memory_order_relaxed)) trace('i', std::uint32_t(mine));
        handle_in_place(g, lock);
        if (const auto trace = switch_trace.load(std::memory_order_relaxed)) trace('j', std::uint32_t(mine));
        if (g->os->suspend > 0) {
            // A handler suspended the thread it interrupted.
            g->os->state = OS_THREAD_STATE_READY;
            switch_away(lock);
            return;
        }
        best = best_slot();
    }
    if (best < mine || (yield && best <= mine)) {
        enqueue(g);
        switch_away(lock);
    }
}

void make_ready(Guest* g) {
    if (g->killed) return;
    if (g->os && g->os->suspend > 0) {
        g->os->state = OS_THREAD_STATE_READY;
        return;
    }
    enqueue(g);
    wake_cpu();
}

Guest* guest_of(OSThread* thread) {
    auto found = guests.find(thread);
    return found == guests.end() ? nullptr : found->second;
}

// The interrupt thread runs the handlers raised while no guest thread held the CPU.
[[noreturn]] void interrupt_loop() {
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
    std::unique_lock<std::mutex> lock(scheduler);
    free_exited();
    trace_resume(&interrupt_guest);
    for (;;) {
        in_handler.store(true, std::memory_order_relaxed);
        while (!pending.empty()) {
            auto handler = next_handler();
            interrupt_guest.interrupts = false;
            lock.unlock();
            handler();
            lock.lock();
        }
        in_handler.store(false, std::memory_order_relaxed);
        trace_release(&interrupt_guest);
        running = nullptr;
        Guest* next = take_next();
        if (next != &interrupt_guest) transfer(interrupt_guest.fiber, next, lock);
        trace_resume(&interrupt_guest);
    }
}

[[noreturn]] void fiber_entry(void* argument) {
    auto* g = static_cast<Guest*>(argument);
    if (g->interrupt_thread) interrupt_loop();
    {
        host_allocation_scope host_;
        std::lock_guard<std::mutex> lock(scheduler);
        free_exited();
        trace_resume(g);
    }
    // Through call_entry, so the asynchronous interrupt signal finds where the thread's own frames end.
    const void* code = g->main ? reinterpret_cast<const void*>(g->main) : reinterpret_cast<const void*>(g->entry);
    const std::uintptr_t result = fiber::call_entry(code, g->parameter, &g->entry_frame);
    if (g->main) main_result = int(result);
    OSExitThread(g->main ? nullptr : reinterpret_cast<void*>(result));
    std::abort();
}

void start_fiber(Guest* g, std::size_t requested) {
    const std::size_t size = std::max(minimum_stack, requested * 4);
    std::snprintf(g->name, sizeof g->name, "guest%d", g->priority());
    g->stack = allocate_stack(size);
    g->stack_size = size;
    fiber::prepare(g->fiber, g->stack, size, fiber_entry, g, g->name);
    g->fiber.sample_id = guest_sample_base + std::uint32_t(slot(g));
}

// The guest CPU's host thread: runs guest threads until the main thread ends, sleeping while none
// is ready.
void cpu_loop() {
    host_allocation_scope host_;
    std::unique_lock<std::mutex> lock(scheduler);
    while (!cpu_stopped) {
        Guest* next = take_next();
        if (!next) {
            cpu_waiting = true;
            cpu_wake.wait(lock);
            cpu_waiting = false;
            continue;
        }
        transfer(idle, next, lock);
    }
}

void init_queue(OSThreadQueue* queue) { queue->head = queue->tail = nullptr; }

void add_by_priority(OSThreadQueue* queue, OSThread* thread) {
    OSThread* next = queue->head;
    while (next && next->priority <= thread->priority) next = next->link.next;
    if (!next) {
        OSThread* prev = queue->tail;
        if (prev) prev->link.next = thread;
        else queue->head = thread;
        thread->link.prev = prev;
        thread->link.next = nullptr;
        queue->tail = thread;
        return;
    }
    thread->link.next = next;
    OSThread* prev = next->link.prev;
    next->link.prev = thread;
    thread->link.prev = prev;
    if (prev) prev->link.next = thread;
    else queue->head = thread;
}

void remove_from(OSThreadQueue* queue, OSThread* thread) {
    OSThread* next = thread->link.next;
    OSThread* prev = thread->link.prev;
    if (next) next->link.prev = prev;
    else queue->tail = prev;
    if (prev) prev->link.next = next;
    else queue->head = next;
    thread->link.next = thread->link.prev = nullptr;
}

void require_guest(const char* call) {
    if (!self) {
        std::fprintf(stderr, "starlet: %s called from a host thread\n", call);
        std::abort();
    }
}

// OSSleepThread with the scheduler locked.
void sleep_on(OSThreadQueue* queue, std::unique_lock<std::mutex>& lock) {
    Guest* g = self;
    if (g->interrupt_thread || handling_in_place) throw std::logic_error("an interrupt handler tried to sleep");
    g->os->state = OS_THREAD_STATE_WAITING;
    g->os->queue = queue;
    add_by_priority(queue, g->os);
    switch_away(lock);
}

void wake_all(OSThreadQueue* queue) {
    while (OSThread* thread = queue->head) {
        remove_from(queue, thread);
        thread->queue = nullptr;
        if (Guest* g = guest_of(thread)) make_ready(g);
    }
}

void terminate(Guest* g, void* value) {
    OSThread* t = g->os;
    t->val = value;
    if (t->queue) {
        remove_from(t->queue, t);
        t->queue = nullptr;
    }
    dequeue(g);
    t->state = (t->attr & OS_THREAD_ATTR_DETACH) ? OS_THREAD_STATE_NULL : OS_THREAD_STATE_MORIBUND;
    wake_all(&t->queueJoin);
    if (t->attr & OS_THREAD_ATTR_DETACH) guests.erase(t);
}

OSThread main_thread;
u8 main_thread_stack_marker[16];

}

void set_switch_trace(SwitchTrace trace) { switch_trace.store(trace, std::memory_order_relaxed); }

void raise_interrupt(std::function<void()> handler) {
    host_allocation_scope host_;
    std::lock_guard<std::mutex> lock(scheduler);
    if (pending.empty() && tracking_pending.load(std::memory_order_relaxed))
        pending_since_ns.store(monotonic_ns(), std::memory_order_relaxed);
    pending.push_back(std::move(handler));
    pending_flag.store(true, std::memory_order_release);
    interrupt_raised.notify_all();
    if (running != &interrupt_guest && !interrupt_guest.queued) enqueue(&interrupt_guest);
    wake_cpu();
}

bool interrupt_pending() { return pending_flag.load(std::memory_order_acquire); }

bool on_guest_thread() { return self != nullptr; }

bool in_interrupt() { return self == &interrupt_guest || handling_in_place; }

std::size_t guest_thread_count() {
    host_allocation_scope host_;
    std::lock_guard<std::mutex> lock(scheduler);
    return guests.size();
}

void wait_for_interrupt() {
    Guest* g = self;
    if (!g || g->interrupt_thread) return;
    {
        host_allocation_scope host_;
        std::unique_lock<std::mutex> lock(scheduler);
        interrupt_raised.wait(lock, [] { return !pending.empty(); });
    }
    checkpoint();
}

void checkpoint() {
    Guest* g = self;
    if (!g || g->interrupt_thread) return;
    // reschedule() leaves the interrupt thread out while this thread has interrupts disabled.
    if (best_ready.load(std::memory_order_acquire) >= slot(g)) return;
    host_allocation_scope host_;
    std::unique_lock<std::mutex> lock(scheduler);
    reschedule(lock, false);
}

int run_main_thread(int (*entry)()) {
    name_thread(guest_cpu_thread_name);
    install_signal_stack();  // A guest stack overflow still gets its crash report.
    {
        host_allocation_scope host_;
        std::lock_guard<std::mutex> lock(scheduler);
        fiber::adopt_current(idle);
        interrupt_guest.interrupt_thread = true;
        interrupt_guest.interrupts = false;
        start_fiber(&interrupt_guest, 0);
    }
    OSCreateThread(&main_thread, nullptr, nullptr, main_thread_stack_marker + sizeof main_thread_stack_marker, 0, 16, 0);
    {
        host_allocation_scope host_;
        std::lock_guard<std::mutex> lock(scheduler);
        main_guest = guest_of(&main_thread);
        main_guest->main = entry;
    }
    cpu_thread.store(pid_t(syscall(SYS_gettid)), std::memory_order_release);
    OSResumeThread(&main_thread);
    cpu_loop();
    cpu_thread.store(0, std::memory_order_release);
    return main_result;
}

std::uint32_t guest_sample_id() { return fiber::current_sample_id(); }

namespace async {

const char* const nothing_pending = "no interrupt pending";

// On the guest CPU thread, from a signal that interrupted its code: the code holds no scheduler
// state unless Starlet's frames are on the stack, which the caller rules out before acting on this.
Interruptee interruptee() {
    Interruptee out;
    const Guest* g = self;
    if (!g) {
        out.refusal = "not a guest thread";
        return out;
    }
    out.name = g->name;
    out.stack_low = reinterpret_cast<std::uintptr_t>(g->stack);
    out.stack_high = out.stack_low + g->stack_size;
    out.entry_frame = reinterpret_cast<std::uintptr_t>(g->entry_frame);
    if (!pending_flag.load(std::memory_order_acquire)) out.refusal = nothing_pending;
    else if (g->interrupt_thread) out.refusal = "the interrupt thread runs";
    else if (handling_in_place) out.refusal = "in an interrupt handler";
    else if (running != g || !g->entry_frame) out.refusal = "switching threads";
    else if (!g->interrupts) out.refusal = "interrupts disabled";
    else if (scheduler_disabled) out.refusal = "scheduler disabled";
    return out;
}

void take_interrupts() {
    host_allocation_scope host_;
    std::unique_lock<std::mutex> lock(scheduler);
    reschedule(lock, false);
}

void track_pending() { tracking_pending.store(true, std::memory_order_relaxed); }

std::uint64_t pending_since() { return pending_since_ns.load(std::memory_order_relaxed); }

pid_t guest_cpu_thread() { return cpu_thread.load(std::memory_order_acquire); }

std::uint64_t now_ns() { return monotonic_ns(); }

}

void report_guest_thread() {
    const Guest* g = self;
    if (!g) return;
    report_text("  on guest thread ");
    report_text(g->name);
    if (g->os) {
        report_text(", OSThread 0x");
        report_hex(reinterpret_cast<std::uintptr_t>(g->os));
    }
    if (handling_in_place) report_text(", in an interrupt handler");
    report_text("\n");
}

}

using namespace starlet::os;

extern "C" {

void OSInitThreadQueue(OSThreadQueue* queue) { init_queue(queue); }

OSThread* OSGetCurrentThread() {
    if (!self) return nullptr;
    if (self->interrupt_thread) return last_guest;
    return self->os;
}

BOOL OSIsThreadTerminated(OSThread* thread) {
    return thread->state == OS_THREAD_STATE_MORIBUND || thread->state == OS_THREAD_STATE_NULL;
}

BOOL OSIsThreadSuspended(OSThread* thread) { return thread->suspend > 0; }

s32 OSDisableScheduler() {
    host_allocation_scope host_;
    std::lock_guard<std::mutex> lock(scheduler);
    return scheduler_disabled++;
}

s32 OSEnableScheduler() {
    host_allocation_scope host_;
    std::unique_lock<std::mutex> lock(scheduler);
    const int previous = scheduler_disabled;
    if (scheduler_disabled > 0) --scheduler_disabled;
    if (!scheduler_disabled) reschedule(lock, false);
    return previous;
}

void OSYieldThread() {
    require_guest("OSYieldThread");
    host_allocation_scope host_;
    std::unique_lock<std::mutex> lock(scheduler);
    reschedule(lock, true);
}

BOOL OSCreateThread(OSThread* thread, OSThreadStartFunction func, void* param, void* stack, u32 stackSize,
                    OSPriority priority, u16 attr) {
    if (priority < OS_PRIORITY_MIN || priority > OS_PRIORITY_MAX) return FALSE;
    host_allocation_scope host;
    auto* g = new Guest;
    g->os = thread;
    g->entry = func;
    g->parameter = param;
    thread->state = OS_THREAD_STATE_READY;
    thread->attr = attr & OS_THREAD_ATTR_DETACH;
    thread->suspend = 1;
    thread->priority = thread->base = priority;
    thread->val = nullptr;
    thread->queue = nullptr;
    thread->link.next = thread->link.prev = nullptr;
    init_queue(&thread->queueJoin);
    thread->mutex = nullptr;
    thread->queueMutex.head = thread->queueMutex.tail = nullptr;
    thread->linkActive.next = thread->linkActive.prev = nullptr;
    thread->stackBase = static_cast<u8*>(stack);
    thread->stackEnd = reinterpret_cast<u32*>(static_cast<u8*>(stack) - stackSize);
    // The SDK marks the end of the game's stack; games check it for overflows. The thread itself
    // runs on a larger native stack, so the game's stack memory only carries the mark.
    if (stack && stackSize >= sizeof(u32)) *thread->stackEnd = OS_THREAD_STACK_MAGIC;
    thread->error = 0;
    thread->specific[0] = thread->specific[1] = nullptr;
    host_allocation_scope host_;
    std::lock_guard<std::mutex> lock(scheduler);
    guests[thread] = g;
    start_fiber(g, stackSize);
    return TRUE;
}

void OSExitThread(void* val) {
    require_guest("OSExitThread");
    host_allocation_scope host_;
    std::unique_lock<std::mutex> lock(scheduler);
    Guest* g = self;
    terminate(g, val);
    end_current(g, lock);
}

void OSCancelThread(OSThread* thread) {
    host_allocation_scope host_;
    std::unique_lock<std::mutex> lock(scheduler);
    Guest* g = guest_of(thread);
    if (!g) return;
    if (g == self) {
        lock.unlock();
        OSExitThread(nullptr);
    }
    g->killed = true;
    terminate(g, nullptr);
    // A thread that holds the CPU (a host thread cancelled it) ends at its next switch; any other
    // never runs again.
    if (g == running) return;
    release_fiber(g);
    if (thread->attr & OS_THREAD_ATTR_DETACH) delete g;
}

void OSDetachThread(OSThread* thread) {
    host_allocation_scope host_;
    std::lock_guard<std::mutex> lock(scheduler);
    thread->attr |= OS_THREAD_ATTR_DETACH;
    if (thread->state == OS_THREAD_STATE_MORIBUND) {
        thread->state = OS_THREAD_STATE_NULL;
        guests.erase(thread);
    }
    wake_all(&thread->queueJoin);
}

s32 OSResumeThread(OSThread* thread) {
    host_allocation_scope host_;
    std::unique_lock<std::mutex> lock(scheduler);
    const s32 previous = thread->suspend;
    if (previous > 0 && --thread->suspend == 0) {
        Guest* g = guest_of(thread);
        if (g && thread->state == OS_THREAD_STATE_READY) make_ready(g);
        reschedule(lock, false);
    }
    return previous;
}

s32 OSSuspendThread(OSThread* thread) {
    host_allocation_scope host_;
    std::unique_lock<std::mutex> lock(scheduler);
    const s32 previous = thread->suspend++;
    if (previous == 0) {
        Guest* g = guest_of(thread);
        if (g && g == self && running == g && !handling_in_place) {
            thread->state = OS_THREAD_STATE_READY;
            switch_away(lock);
        } else if (g) {
            dequeue(g);
        }
    }
    return previous;
}

void OSSleepThread(OSThreadQueue* queue) {
    require_guest("OSSleepThread");
    host_allocation_scope host_;
    std::unique_lock<std::mutex> lock(scheduler);
    sleep_on(queue, lock);
}

void OSWakeupThread(OSThreadQueue* queue) {
    host_allocation_scope host_;
    std::unique_lock<std::mutex> lock(scheduler);
    wake_all(queue);
    reschedule(lock, false);
}

void OSClearStack(u8) {}

OSPriority OSGetThreadPriority(OSThread* thread) { return thread->base; }

BOOL OSSetThreadPriority(OSThread* thread, OSPriority priority) {
    if (priority < OS_PRIORITY_MIN || priority > OS_PRIORITY_MAX) return FALSE;
    host_allocation_scope host_;
    std::unique_lock<std::mutex> lock(scheduler);
    Guest* g = guest_of(thread);
    const bool queued = g && g->queued;
    if (queued) dequeue(g);
    thread->base = thread->priority = priority;
    if (queued) enqueue(g);
    if (thread->queue) {
        remove_from(thread->queue, thread);
        add_by_priority(thread->queue, thread);
    }
    reschedule(lock, false);
    return TRUE;
}

BOOL OSJoinThread(OSThread* thread, void** val) {
    host_allocation_scope host_;
    std::unique_lock<std::mutex> lock(scheduler);
    if (!(thread->attr & OS_THREAD_ATTR_DETACH) && thread->state != OS_THREAD_STATE_MORIBUND &&
        thread->state != OS_THREAD_STATE_NULL)
        sleep_on(&thread->queueJoin, lock);
    if (thread->state != OS_THREAD_STATE_MORIBUND) return FALSE;
    if (val) *val = thread->val;
    thread->state = OS_THREAD_STATE_NULL;
    if (Guest* g = guest_of(thread)) {
        // It ended (its stack is free) or was cancelled.
        guests.erase(thread);
        release_fiber(g);
        delete g;
    }
    return TRUE;
}

s32 OSCheckActiveThreads() {
    host_allocation_scope host_;
    std::lock_guard<std::mutex> lock(scheduler);
    return static_cast<s32>(guests.size());
}

BOOL OSDisableInterrupts() {
    if (!self) return FALSE;
    const bool previous = self->interrupts;
    self->interrupts = false;
    return previous;
}

BOOL OSEnableInterrupts() {
    if (!self) return TRUE;
    const bool previous = self->interrupts;
    self->interrupts = true;
    checkpoint();
    return previous;
}

BOOL OSRestoreInterrupts(BOOL level) {
    if (!self) return level;
    const bool previous = self->interrupts;
    self->interrupts = level != FALSE;
    if (level) checkpoint();
    return previous;
}

void OSInitMutex(OSMutex* mutex) {
    init_queue(&mutex->queue);
    mutex->thread = nullptr;
    mutex->count = 0;
    mutex->link.next = mutex->link.prev = nullptr;
}

static void own_mutex(OSMutex* mutex, OSThread* owner) {
    mutex->thread = owner;
    mutex->count = 1;
    OSMutex* prev = owner->queueMutex.tail;
    mutex->link.prev = prev;
    mutex->link.next = nullptr;
    if (prev) prev->link.next = mutex;
    else owner->queueMutex.head = mutex;
    owner->queueMutex.tail = mutex;
}

static void disown_mutex(OSMutex* mutex) {
    OSThread* owner = mutex->thread;
    OSMutex* next = mutex->link.next;
    OSMutex* prev = mutex->link.prev;
    if (next) next->link.prev = prev;
    else owner->queueMutex.tail = prev;
    if (prev) prev->link.next = next;
    else owner->queueMutex.head = next;
    mutex->thread = nullptr;
    mutex->count = 0;
}

void OSLockMutex(OSMutex* mutex) {
    require_guest("OSLockMutex");
    host_allocation_scope host_;
    std::unique_lock<std::mutex> lock(scheduler);
    OSThread* me = self->os;
    for (;;) {
        if (!mutex->thread) {
            own_mutex(mutex, me);
            return;
        }
        if (mutex->thread == me) {
            ++mutex->count;
            return;
        }
        me->mutex = mutex;
        sleep_on(&mutex->queue, lock);
        me->mutex = nullptr;
    }
}

void OSUnlockMutex(OSMutex* mutex) {
    host_allocation_scope host_;
    std::unique_lock<std::mutex> lock(scheduler);
    if (!self || mutex->thread != self->os || --mutex->count > 0) return;
    disown_mutex(mutex);
    wake_all(&mutex->queue);
    reschedule(lock, false);
}

BOOL OSTryLockMutex(OSMutex* mutex) {
    host_allocation_scope host_;
    std::lock_guard<std::mutex> lock(scheduler);
    OSThread* me = self ? self->os : nullptr;
    if (!mutex->thread) {
        own_mutex(mutex, me);
        return TRUE;
    }
    if (mutex->thread == me) {
        ++mutex->count;
        return TRUE;
    }
    return FALSE;
}

void OSInitCond(OSCond* cond) { init_queue(&cond->queue); }

void OSWaitCond(OSCond* cond, OSMutex* mutex) {
    require_guest("OSWaitCond");
    host_allocation_scope host_;
    std::unique_lock<std::mutex> lock(scheduler);
    OSThread* me = self->os;
    if (mutex->thread != me) return;
    const int count = mutex->count;
    disown_mutex(mutex);
    wake_all(&mutex->queue);
    sleep_on(&cond->queue, lock);
    for (;;) {
        if (!mutex->thread) break;
        me->mutex = mutex;
        sleep_on(&mutex->queue, lock);
        me->mutex = nullptr;
    }
    own_mutex(mutex, me);
    mutex->count = count;
}

void OSSignalCond(OSCond* cond) { OSWakeupThread(&cond->queue); }

void OSInitMessageQueue(OSMessageQueue* queue, OSMessage* msgArray, s32 msgCount) {
    init_queue(&queue->queueSend);
    init_queue(&queue->queueReceive);
    queue->msgArray = msgArray;
    queue->msgCount = msgCount;
    queue->firstIndex = 0;
    queue->usedCount = 0;
}

BOOL OSSendMessage(OSMessageQueue* queue, OSMessage msg, s32 flags) {
    host_allocation_scope host_;
    std::unique_lock<std::mutex> lock(scheduler);
    while (queue->msgCount <= queue->usedCount) {
        if (!(flags & OS_MESSAGE_BLOCK)) return FALSE;
        require_guest("OSSendMessage");
        sleep_on(&queue->queueSend, lock);
    }
    queue->msgArray[(queue->firstIndex + queue->usedCount) % queue->msgCount] = msg;
    ++queue->usedCount;
    wake_all(&queue->queueReceive);
    reschedule(lock, false);
    return TRUE;
}

BOOL OSJamMessage(OSMessageQueue* queue, OSMessage msg, s32 flags) {
    host_allocation_scope host_;
    std::unique_lock<std::mutex> lock(scheduler);
    while (queue->msgCount <= queue->usedCount) {
        if (!(flags & OS_MESSAGE_BLOCK)) return FALSE;
        require_guest("OSJamMessage");
        sleep_on(&queue->queueSend, lock);
    }
    queue->firstIndex = (queue->firstIndex + queue->msgCount - 1) % queue->msgCount;
    queue->msgArray[queue->firstIndex] = msg;
    ++queue->usedCount;
    wake_all(&queue->queueReceive);
    reschedule(lock, false);
    return TRUE;
}

BOOL OSReceiveMessage(OSMessageQueue* queue, OSMessage* msgPtr, s32 flags) {
    host_allocation_scope host_;
    std::unique_lock<std::mutex> lock(scheduler);
    while (queue->usedCount == 0) {
        if (!(flags & OS_MESSAGE_BLOCK)) return FALSE;
        require_guest("OSReceiveMessage");
        sleep_on(&queue->queueReceive, lock);
    }
    if (msgPtr) *msgPtr = queue->msgArray[queue->firstIndex];
    queue->firstIndex = (queue->firstIndex + 1) % queue->msgCount;
    --queue->usedCount;
    wake_all(&queue->queueSend);
    reschedule(lock, false);
    return TRUE;
}

}
