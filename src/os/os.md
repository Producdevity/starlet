# starlet::os

The Dolphin OS for SDK-level ports: the game's SDK calls bind by name to the C-linkage functions
declared in `include/starlet/sdk/os.h`. Moved from the Pikmin 2 port's platform layer.

- `guest_cpu`: the console's single CPU. Guest threads (OSThread) are fibers on one host thread,
  the guest CPU thread (`guest-cpu`, the thread that calls `run_main_thread`): each has its own
  stack from the guest stack pool, and handing the CPU to another guest thread swaps stacks and
  registers (`fiber.cpp`: AArch64 and x86-64 assembly) instead of waking a host thread. Threads
  take turns by OS priority; a switch happens inside an OS call or at a checkpoint that finds an
  interrupt pending. Host service threads queue interrupt handlers with `raise_interrupt`. The
  running thread takes them at its next checkpoint or OS call while it has interrupts enabled, as
  the console's CPU does, and a thread they wake with a better priority runs before it goes on.
  While no thread runs, an interrupt thread (a fiber too) runs them. The guest CPU thread sleeps
  only while no guest thread is ready; `raise_interrupt` and wakeups from host threads wake it.
  `wait_for_interrupt` is for game loops that spin on a variable an interrupt handler sets.
  `set_switch_trace` reports each hand-over of the CPU for diagnostics (the GX frame trace records
  them).
  - A switch saves with the context what belongs to the code rather than the host thread: errno,
    the C++ runtime's caught-exception chain, `host_allocation` routing and the profiler's sample
    id. Other thread-local state is shared by all guest threads and holds only values that no
    OS call spans (`handling_in_place`, the GX sync-stats context, the write-gather buffer,
    `exiting_here` in host/quit.cpp).
  - Guest code must not hold a host mutex across an OS call (the next guest thread on the same
    host thread would deadlock on it, as it did when each guest thread had its own host thread).
    A host wait inside guest code (GX readbacks and drains, DMA waits, `wait_for_interrupt`)
    holds the guest CPU, as it did before; disc, card and audio work runs on host threads and
    completes through `raise_interrupt`, and sync callers sleep with OSSleepThread.
  - A thread that ends is abandoned without unwinding its frames; the next context to run frees
    its stack. Profiles name samples on the guest CPU thread "guest<priority>"; crash reports
    name the guest thread, and the guest CPU thread has its own signal stack, so a guest stack
    overflow is reported. Under TSan each guest thread is a TSan fiber.
- `async_interrupts`: interrupts at any instruction, as the console's CPU takes them, for game
  loops that spin on state only an interrupt handler changes. Off until the port calls
  `add_guest_code` with the game's code and `enable_async_interrupts` at start-up.
  - A watchdog thread (`irq-watchdog`) looks every half threshold (20 ms by default). Once
    interrupts have been pending for the threshold and the guest CPU thread used at least half
    the time since the last look (a thread blocked in a system call is left alone), it sends that
    thread SIGURG every 2 ms until they are taken.
  - The handler runs on the interrupted fiber's stack and takes the interrupts as a checkpoint
    does, a hand-over to a better thread they ready included, only at a safe point: a guest
    thread with interrupts and the scheduler enabled, outside interrupt handlers, whose program
    counter, link register and frame-record return addresses down to its entry lie in registered
    code. The bottom record is the one `fiber::call_entry` makes when the thread starts; its
    callee (the function given to `run_main_thread` or OSCreateThread) is checked too. Elsewhere
    the handler notes the attempt and returns, and the next signal retries.
  - The check is sound for any host code because registered code keeps frame records: a host
    function below guest code shows as a return address, in the record of the guest function it
    called, or in the link register while that function has no record yet or any more (a leaf,
    a prologue, an epilogue). At a safe point the thread holds no host lock and no half-updated
    host state, as at a checkpoint, so handlers lock, allocate and switch fibers as usual.
  - Signal state: the handler saves and restores errno and unblocks SIGURG before taking the
    interrupts, since a hand-over postpones its return and the next thread needs the signal;
    sigreturn restores the interrupted registers and signal mask. A switch inside it saves the
    context as any switch does.
  - Registered code is code the console's CPU may interrupt anywhere: the game's, built with
    `-fno-omit-frame-pointer`. Between its instructions it holds no host lock (the rule for OS
    calls, extended to every instruction while interrupts are enabled). A spin on a variable the
    compiler keeps in a register stays stuck: the game declares such variables volatile.
  - Logs: `[os] took interrupts asynchronously in guest thread NAME at PC 0x... (module+0x...)`
    once per program counter, and after 1 s without a safe point
    `[os] no safe point in 1 s to take pending interrupts in guest thread NAME: REASON` with the
    pc, lr and return addresses, once per stall and program counter.
  - AArch64 Linux only. Off under TSan, which runs signal handlers later at its own interception
    points with the context of the original interruption.
- `os_core`: arena bounds, time base (40.5 MHz ticks), calendar, alarms, reports, cache maintenance
  (reported to observers such as the GX), console settings, reset. Host transfers into and out of game
  memory (ARAM, disc, card, THP, audio DMA) call `before_dma_read` or `before_dma_write` first, and
  report what they wrote with `report_cpu_write`; cache stores and flushes are not transfers.
- `os_misc`: processor registers, exception context, error handlers, locked cache switch, the IPL
  ROM font geometry with blank glyphs.
- `game_memory`: the port's memory map. Physical address p is `window_base + p`, except the
  executable image's own physical addresses, which resolve to the image; the native range they
  would name (`image_alias`) holds no other game memory. The arena, fixed console regions
  (`map_fixed`) and the guest stack pool are game memory that hardware addresses resolve to.
- `host_allocation`: global new and delete by thread: guest threads use the game's heap once the
  port installs it, host code uses malloc; `host_allocation_scope` marks host work on guest threads.
- `crash_report`: async-signal-safe reports for players' logs, and the fatal signal handler.

A port checks its decompilation's SDK layouts against `starlet/sdk/os.h` by including it with
`STARLET_SDK_TYPES_ONLY` before its own headers and asserting sizes and offsets.

Linux only. Test: `tests/os` (threads, priorities, messages, mutexes, conditions, interrupts,
alarms, disabled interrupts, waiting for an interrupt, switching, interrupts that wake the idle
guest CPU thread, preemption; spins that only an interrupt handler ends, with and without
asynchronous interrupts, host frames and disabled interrupts that defer them, their logs; the
image's physical addresses; crash reports after another handler raised the signal again, and
on guest threads).
