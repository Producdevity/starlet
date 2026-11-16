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
guest CPU thread, preemption; the image's physical addresses; crash reports after another
handler raised the signal again, and on guest threads).
