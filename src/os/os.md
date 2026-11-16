# starlet::os

The Dolphin OS for SDK-level ports: the game's SDK calls bind by name to the C-linkage functions
declared in `include/starlet/sdk/os.h`. Moved from the Pikmin 2 port's platform layer.

- `guest_cpu`: the console's single CPU. Guest threads (OSThread) are host threads that take turns
  by OS priority; a switch happens inside an OS call or at a checkpoint that finds an interrupt
  pending. Host service threads queue interrupt handlers with `raise_interrupt`. The running thread
  takes them at its next checkpoint or OS call while it has interrupts enabled, as the console's CPU
  does: the handlers run on its host thread, and a thread they wake with a better priority runs
  before it goes on. While no thread runs, an interrupt thread runs them. Every hand-over between
  host threads costs a Linux wakeup (on H700 about 35 us on average, sometimes over 1 ms); a game
  takes dozens of interrupts per frame. `wait_for_interrupt` is for game loops that spin on a
  variable an interrupt handler sets. `set_switch_trace` reports each hand-over of the CPU for
  diagnostics (the GX frame trace records them).
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
alarms, disabled interrupts, waiting for an interrupt; the image's physical addresses; crash
reports after another handler raised the signal again).
