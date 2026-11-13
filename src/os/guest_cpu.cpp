#include "starlet/os/guest_cpu.hpp"

#include "starlet/os/game_memory.hpp"
#include "starlet/os/host_allocation.hpp"
#include "starlet/os/thread_name.hpp"

#include "starlet/sdk/os.h"

#include <pthread.h>

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
#include <vector>

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
    pthread_t host{};
    std::condition_variable turn;
    bool interrupts = true;  // MSR[EE] of this thread's context.
    bool interrupt_thread = false;
    bool queued = false;      // In a ready slot.
    bool started = false;     // Host thread exists.
    bool killed = false;      // Cancelled: the host thread ends when it next wakes.
    bool reap = true;         // The host thread hands its stack back when it ends (not the main thread).
    bool retired = false;     // The host thread queued itself for reaping; no one else joins it.
    void* stack = nullptr;
    std::size_t stack_size = 0;
    void* (*entry)(void*) = nullptr;
    void* parameter = nullptr;
    int priority() const { return interrupt_thread ? interrupt_priority : os->priority; }
};

std::mutex scheduler;
Guest* running = nullptr;
std::array<std::deque<Guest*>, priority_slots> ready;
std::atomic<int> best_ready{priority_slots};  // Slot index of the best ready thread.
std::deque<std::function<void()>> pending;
std::condition_variable interrupt_raised;
std::atomic<bool> pending_flag{false};
std::unordered_map<OSThread*, Guest*> guests;
int scheduler_disabled = 0;
Guest interrupt_guest;
OSThread* interrupted = nullptr;  // The guest thread an interrupt delivery preempted.
OSThread* last_guest = nullptr;
thread_local Guest* self = nullptr;

// Host threads that ended: the next thread creation joins them and reuses their stacks.
struct Finished {
    pthread_t host;
    void* stack;
    std::size_t size;
};
std::mutex finished_mutex;
std::vector<Finished> finished;

// Under the scheduler lock, before the host thread lets go of it.
Finished retire(Guest* g) {
    g->retired = g->reap;
    return {pthread_self(), g->reap ? g->stack : nullptr, g->stack_size};
}

[[noreturn]] void exit_host(const Finished& done) {
    if (done.stack) {
        host_allocation_scope host_;
        std::lock_guard<std::mutex> lock(finished_mutex);
        finished.push_back(done);
    }
    pthread_exit(nullptr);
}

void reap_finished() {
    std::vector<Finished> done;
    {
        host_allocation_scope host_;
        std::lock_guard<std::mutex> lock(finished_mutex);
        done.swap(finished);
    }
    for (const Finished& f : done) {
        pthread_join(f.host, nullptr);
        release_stack(f.stack, f.size);
    }
}
std::atomic<bool> in_handler{false};

int slot(const Guest* g) { return g->priority() + 1; }

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

// The CPU is free: hand it to the best ready thread, if any.
void dispatch() {
    if (running) throw std::logic_error("dispatch with the CPU held");
    for (int i = 0; i < priority_slots; ++i) {
        if (ready[i].empty()) continue;
        Guest* next = ready[i].front();
        ready[i].pop_front();
        next->queued = false;
        update_best();
        running = next;
        if (!next->interrupt_thread) {
            next->os->state = OS_THREAD_STATE_RUNNING;
            last_guest = next->os;
        }
        next->turn.notify_one();
        return;
    }
}

[[noreturn]] void end_host_thread(std::unique_lock<std::mutex>& lock) {
    Guest* g = self;
    const Finished done = retire(g);
    // A cancelled detached thread is out of the table; no one else holds it.
    const bool detached = g->os && (g->os->attr & OS_THREAD_ATTR_DETACH);
    lock.unlock();
    if (detached) delete g;
    exit_host(done);
}

void wait_turn(Guest* g, std::unique_lock<std::mutex>& lock) {
    while (running != g) {
        if (g->killed) end_host_thread(lock);
        g->turn.wait(lock);
    }
}

// Gives up the CPU (the caller has already queued or parked itself) and waits
// until it is dispatched again.
void switch_away(std::unique_lock<std::mutex>& lock) {
    Guest* g = self;
    running = nullptr;
    dispatch();
    wait_turn(g, lock);
}

// Lets a better thread run. A yield also gives way to equal priority. The
// interrupt thread is eligible only while this thread has interrupts enabled.
void reschedule(std::unique_lock<std::mutex>& lock, bool yield) {
    Guest* g = self;
    if (!g || running != g || g->interrupt_thread) return;
    if (scheduler_disabled && !yield) return;
    const int mine = slot(g);
    int best = priority_slots;
    for (int i = g->interrupts ? 0 : 1; i < priority_slots; ++i)
        if (!ready[i].empty()) { best = i; break; }
    if (best < mine || (yield && best <= mine)) {
        if (best == 0) interrupted = g->os;
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
    if (!running) dispatch();
}

Guest* guest_of(OSThread* thread) {
    auto found = guests.find(thread);
    return found == guests.end() ? nullptr : found->second;
}

void interrupt_loop() {
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
    std::unique_lock<std::mutex> lock(scheduler);
    for (;;) {
        wait_turn(&interrupt_guest, lock);
        in_handler.store(true, std::memory_order_relaxed);
        while (!pending.empty()) {
            auto handler = std::move(pending.front());
            pending.pop_front();
            pending_flag.store(!pending.empty(), std::memory_order_release);
            interrupt_guest.interrupts = false;
            lock.unlock();
            handler();
            lock.lock();
        }
        in_handler.store(false, std::memory_order_relaxed);
        interrupted = nullptr;
        running = nullptr;
        dispatch();
    }
}

void* host_entry(void* argument) {
    auto* g = static_cast<Guest*>(argument);
    self = g;
    set_guest_allocation(true);
    {
        char name[16];
        std::snprintf(name, sizeof name, "guest%d", g->priority());
        name_thread(name);
    }
    if (g->interrupt_thread) {
        interrupt_loop();
        return nullptr;
    }
    {
        host_allocation_scope host_;  // Scheduler containers must not take game memory.
        std::unique_lock<std::mutex> lock(scheduler);
        wait_turn(g, lock);
    }
    void* result = g->entry(g->parameter);
    OSExitThread(result);
    return nullptr;
}

void start_host_thread(Guest* g, std::size_t requested) {
    const std::size_t size = std::max(minimum_stack, requested * 4);
    reap_finished();
    g->stack = allocate_stack(size);
    g->stack_size = size;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstack(&attributes, g->stack, size);
    const int error = pthread_create(&g->host, &attributes, host_entry, g);
    pthread_attr_destroy(&attributes);
    if (error) throw std::runtime_error("could not create a guest thread");
    g->started = true;
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
    if (g->interrupt_thread) throw std::logic_error("an interrupt handler tried to sleep");
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

void raise_interrupt(std::function<void()> handler) {
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
    std::lock_guard<std::mutex> lock(scheduler);
    pending.push_back(std::move(handler));
    pending_flag.store(true, std::memory_order_release);
    interrupt_raised.notify_all();
    if (running != &interrupt_guest && !interrupt_guest.queued) enqueue(&interrupt_guest);
    if (!running) dispatch();
}

bool interrupt_pending() { return pending_flag.load(std::memory_order_acquire); }

bool on_guest_thread() { return self != nullptr; }

bool in_interrupt() { return self == &interrupt_guest; }

std::size_t guest_thread_count() {
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
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
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
    std::unique_lock<std::mutex> lock(scheduler);
    reschedule(lock, false);
}

int run_main_thread(int (*entry)()) {
    static int (*main_entry)() = nullptr;
    static int result = 0;
    main_entry = entry;
    {
        host_allocation_scope host_;  // Scheduler containers must not take game memory.
        std::lock_guard<std::mutex> lock(scheduler);
        interrupt_guest.interrupt_thread = true;
        interrupt_guest.interrupts = false;
        start_host_thread(&interrupt_guest, 0);
    }
    OSCreateThread(&main_thread, [](void*) -> void* {
        result = main_entry();
        return nullptr;
    }, nullptr, main_thread_stack_marker + sizeof main_thread_stack_marker, 0, 16, 0);
    Guest* g = guest_of(&main_thread);
    g->reap = false;  // Joined below.
    OSResumeThread(&main_thread);
    pthread_join(g->host, nullptr);
    return result;
}

}

using namespace starlet::os;

extern "C" {

void OSInitThreadQueue(OSThreadQueue* queue) { init_queue(queue); }

OSThread* OSGetCurrentThread() {
    if (!self) return nullptr;
    if (self->interrupt_thread) return interrupted ? interrupted : last_guest;
    return self->os;
}

BOOL OSIsThreadTerminated(OSThread* thread) {
    return thread->state == OS_THREAD_STATE_MORIBUND || thread->state == OS_THREAD_STATE_NULL;
}

BOOL OSIsThreadSuspended(OSThread* thread) { return thread->suspend > 0; }

s32 OSDisableScheduler() {
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
    std::lock_guard<std::mutex> lock(scheduler);
    return scheduler_disabled++;
}

s32 OSEnableScheduler() {
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
    std::unique_lock<std::mutex> lock(scheduler);
    const int previous = scheduler_disabled;
    if (scheduler_disabled > 0) --scheduler_disabled;
    if (!scheduler_disabled) reschedule(lock, false);
    return previous;
}

void OSYieldThread() {
    require_guest("OSYieldThread");
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
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
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
    std::lock_guard<std::mutex> lock(scheduler);
    guests[thread] = g;
    start_host_thread(g, stackSize);
    return TRUE;
}

void OSExitThread(void* val) {
    require_guest("OSExitThread");
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
    std::unique_lock<std::mutex> lock(scheduler);
    Guest* g = self;
    terminate(g, val);
    running = nullptr;
    dispatch();
    // A joiner may delete g once the lock is released.
    const bool detached = g->os->attr & OS_THREAD_ATTR_DETACH;
    const Finished done = retire(g);
    lock.unlock();
    if (detached) delete g;
    exit_host(done);
}

void OSCancelThread(OSThread* thread) {
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
    std::unique_lock<std::mutex> lock(scheduler);
    Guest* g = guest_of(thread);
    if (!g) return;
    if (g == self) {
        lock.unlock();
        OSExitThread(nullptr);
    }
    g->killed = true;
    terminate(g, nullptr);
    g->turn.notify_one();
}

void OSDetachThread(OSThread* thread) {
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
    std::lock_guard<std::mutex> lock(scheduler);
    thread->attr |= OS_THREAD_ATTR_DETACH;
    if (thread->state == OS_THREAD_STATE_MORIBUND) {
        thread->state = OS_THREAD_STATE_NULL;
        guests.erase(thread);
    }
    wake_all(&thread->queueJoin);
}

s32 OSResumeThread(OSThread* thread) {
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
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
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
    std::unique_lock<std::mutex> lock(scheduler);
    const s32 previous = thread->suspend++;
    if (previous == 0) {
        Guest* g = guest_of(thread);
        if (g && g == self && running == g) {
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
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
    std::unique_lock<std::mutex> lock(scheduler);
    sleep_on(queue, lock);
}

void OSWakeupThread(OSThreadQueue* queue) {
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
    std::unique_lock<std::mutex> lock(scheduler);
    wake_all(queue);
    reschedule(lock, false);
}

void OSClearStack(u8) {}

OSPriority OSGetThreadPriority(OSThread* thread) { return thread->base; }

BOOL OSSetThreadPriority(OSThread* thread, OSPriority priority) {
    if (priority < OS_PRIORITY_MIN || priority > OS_PRIORITY_MAX) return FALSE;
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
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
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
    std::unique_lock<std::mutex> lock(scheduler);
    if (!(thread->attr & OS_THREAD_ATTR_DETACH) && thread->state != OS_THREAD_STATE_MORIBUND &&
        thread->state != OS_THREAD_STATE_NULL)
        sleep_on(&thread->queueJoin, lock);
    if (thread->state != OS_THREAD_STATE_MORIBUND) return FALSE;
    if (val) *val = thread->val;
    thread->state = OS_THREAD_STATE_NULL;
    if (Guest* g = guest_of(thread)) {
        guests.erase(thread);
        if (g->retired) {
            // Its host thread queued itself for reaping and no longer touches g.
            lock.unlock();
            delete g;
            return TRUE;
        }
        g->reap = false;
        pthread_t host = g->host;
        lock.unlock();
        pthread_join(host, nullptr);
        release_stack(g->stack, g->stack_size);
        delete g;
    }
    return TRUE;
}

s32 OSCheckActiveThreads() {
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
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
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
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
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
    std::unique_lock<std::mutex> lock(scheduler);
    if (!self || mutex->thread != self->os || --mutex->count > 0) return;
    disown_mutex(mutex);
    wake_all(&mutex->queue);
    reschedule(lock, false);
}

BOOL OSTryLockMutex(OSMutex* mutex) {
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
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
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
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
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
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
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
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
    host_allocation_scope host_;  // Scheduler containers must not take game memory.
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
