# OS and GX FIFO

Two Linux-only libraries for SDK-level ports. `starlet::os` is the Dolphin OS: the game's SDK calls bind by
name to the C-linkage functions declared in `starlet/sdk/os.h`. `starlet::gx_fifo` (`starlet_gx_fifo`)
implements the GX calls that drive hardware (write-gather pipe, FIFO objects, synchronization, EFB peeks) and
the VI. Renderer side of peeks and copies: [GLES renderer](GLES-renderer.md). Command decoding and state:
[GX model](GX-model.md). DVD, PAD, ARAM, NAND, THP, AI and AX: [SDK services](SDK-services.md).

A port checks its decompilation's SDK layouts against `starlet/sdk/os.h` by including it with
`STARLET_SDK_TYPES_ONLY` before its own headers and asserting sizes and offsets.

## Guest threads and fibers

The console has one CPU. Guest threads (`OSThread`) are fibers on one host thread, the guest CPU thread
(`guest-cpu`, the thread that calls `run_main_thread`). Each has its own stack from the guest stack pool.
Handing the CPU over swaps stacks and registers (`fiber.cpp`, AArch64 and x86-64 assembly) with no kernel wakeup.
Game code never runs concurrently with game code.

- Scheduling follows OS priority, as the Dolphin OS scheduler does. `run_main_thread` starts the main thread at
  priority 16.
- A switch happens inside an OS call (block, wakeup of a better thread, yield) or at a `checkpoint()` that finds
  an interrupt pending. Long paths that make no OS call (the GX FIFO) call `checkpoint()`.
- The guest CPU thread sleeps only while no guest thread is ready. `raise_interrupt` and wakeups from host
  threads wake it.
- A switch saves what belongs to the code rather than the host thread: errno, the C++ runtime's caught-exception
  chain, `host_allocation` routing and the profiler's sample id. Other thread-local state is shared by all guest
  threads and holds only values that no OS call spans (`handling_in_place`, the GX sync-stats context, the
  write-gather buffer, `exiting_here` in `host/quit.cpp`).
- A thread that ends is abandoned without unwinding its frames; the next context to run frees its stack.
  Cancelling a thread that holds the CPU ends it at its next switch.
- Native frames are larger than the console's and host library calls (stdio, SDL, GL) run on guest stacks, so a
  stack is at least 1 MiB. The game's own stack memory only carries the end-of-stack mark games check for
  overflow.
- Under TSan each guest thread is a TSan fiber.
- `OSSetSwitchThreadCallback` runs the game's callback, with the scheduler held, each time the CPU passes from one OS
  thread to another (JSystem's JKRThreadSwitch keeps a current heap per thread with it). Interrupt fibers do not count.
- `set_switch_trace` reports each hand-over of the CPU for diagnostics; the GX frame trace records them (kinds
  `h`, `g`, `x`, `i`, `j`, see `starlet/gx_fifo/frame_trace.hpp`).

Thread ownership:

- Guest code must not hold a host mutex across an OS call: the next guest thread on the same host thread would
  deadlock on it.
- A host wait inside guest code (GX readbacks and drains, DMA waits, `wait_for_interrupt`) holds the guest CPU.
  Disc, card and audio work runs on host threads and completes through `raise_interrupt`; synchronous callers
  sleep with `OSSleepThread`.
- Scheduler containers allocate from the host (`host_allocation_scope`), never from the game's heap.
- The GPU and GL threads never wait for a guest or DMA thread.

## Interrupts

Host service threads (timers, disc, audio) queue handlers with `raise_interrupt`. Handlers run in order with
the CPU held and interrupts disabled.

- The running thread takes them at its next checkpoint or OS call while it has interrupts enabled, as the
  console's CPU does. A thread they wake with a better priority runs before the interrupted one goes on.
- While no thread runs, an interrupt thread (a fiber too) runs them.
- `wait_for_interrupt` is for game loops that spin on a variable a handler sets (a retrace count): the console's
  CPU took the interrupt in the middle of the loop.
- A guest thread waiting for the GPU thread (host transfers, `GXDrawDone`, peeks) takes pending interrupts during
  the wait, as the console's CPU did, so the audio DMA interrupt is not held for the whole wait.
- Interrupts are taken only at checkpoints: a spin loop that never calls an OS call freezes, and a gap over about
  12 ms makes AI replay audio. Asynchronous interrupts below remove the first problem.

### Asynchronous interrupts

The console's CPU takes an interrupt at any instruction. `enable_async_interrupts` does the same for game loops
that spin on state only a handler changes. Off until the port calls `add_guest_code` with the game's code and
`enable_async_interrupts` at start-up. AArch64 Linux only; off under TSan, which runs signal handlers later at its
own interception points with the context of the original interruption, so the frame check would vouch for code
that is no longer running.

- A watchdog thread (`irq-watchdog`) looks every half threshold (20 ms by default). Once interrupts have been
  pending for the threshold and the guest CPU thread used at least half the time since the last look (a thread
  blocked in a system call is left alone), it sends that thread SIGURG every 2 ms until they are taken.
- The handler runs on the interrupted fiber's stack. It takes the interrupts as a checkpoint does, a hand-over
  to a better thread included, only at a safe point: a guest thread with interrupts and the scheduler enabled,
  outside interrupt handlers, whose pc, lr and frame-record return addresses down to its entry lie in registered
  code. The bottom record is the one `fiber::call_entry` makes when the thread starts; its callee (the function
  given to `run_main_thread` or `OSCreateThread`) is checked too. Elsewhere the handler notes the attempt and
  returns, and the next signal retries.
- The check is sound for any host code because registered code keeps frame records: a host function below guest
  code shows as a return address, in the record of the guest function it called, or in the link register while
  that function has no record yet or any more (a leaf, a prologue, an epilogue). At a safe point the thread holds
  no host lock and no half-updated host state, as at a checkpoint, so handlers lock, allocate and switch fibers
  as usual.
- The handler saves and restores errno and unblocks SIGURG before taking the interrupts, since a hand-over
  postpones its return and the next thread needs the signal. sigreturn restores the interrupted registers and
  signal mask.
- Registered code is code the console's CPU may interrupt anywhere: the game's, built with
  `-fno-omit-frame-pointer`, holding no host lock between instructions. At most 16 ranges; a range holding
  Starlet's own code is refused and logged. A spin on a variable the compiler keeps in a register stays stuck: the
  game declares such variables volatile.
- Logs: `[os] took interrupts asynchronously in guest thread NAME at PC 0x... (module+0x...)` once per program
  counter, and after 1 s without a safe point `[os] no safe point in 1 s to take pending interrupts in guest
  thread NAME: REASON` with pc, lr and return addresses, once per stall and program counter.
- `guest_thread_preempted()` is true while a guest thread is stopped at an arbitrary instruction, where it may
  hold game memory in registers (for example `starlet_gx_stage.cur`). While one is stopped, a GX flush submits a
  copy of the new bytes and leaves the stage in place; making room for a writer moves it anyway, as two threads
  writing at once corrupt the console's FIFO too.

## Alarms and time

- The time base counts 40.5 MHz ticks from boot. The calendar is the host's local time at boot plus elapsed ticks,
  from the year 2000 as the console counts.
- The bus and core clocks are written to low memory, where the SDK's `OS_BUS_CLOCK` and `OS_CORE_CLOCK` read
  them natively.
- Alarms fire on the timer thread as interrupts, in fire order. A handler skips an alarm cancelled or set again
  before it runs on the guest CPU, as the console's queue would. A periodic alarm's first expiry is the next
  `start + n * period` after now, as the SDK computes it.
- VI retraces run at 60000/1001 Hz. After a long stall (debugger, suspend) they resume at the current time
  instead of delivering a burst.

## Game memory

Every address the game holds must fit in 32 bits: decompilations store pointers in `u32` fields, and GX, DMA and
DSP addresses are 32-bit. A port links without PIE and keeps the executable image, arena, guest stacks and fixed
console regions below 4 GiB.

- Hardware decodes physical addresses. The game converts with `OSCachedToPhysical` (pointer - 0x80000000) or
  `OSCachedToUncached` (pointer + 0x40000000). Physical address `p` is `window_base + p`: 0x80000000 for a port
  whose memory sits at the console's addresses, 0 for one that sits low.
- GX copy destination and TLUT load registers hold an address shifted right by 5 in 21 bits, so what the GX
  writes or loads that way must lie in the first 64 MiB of the window.
- The executable image keeps its link address, so its physical addresses need not be `window_base + p`: with
  `window_base` 0x80000000 and an image at 0x200000, a GX copy into a static buffer at 0x700000 carries physical
  0x700000, which would name 0x80700000. Physical addresses of the image resolve to the image, and the native range
  they would name (`image_alias`) holds no other game memory: `set_arena`, `map_fixed` and `set_stack_pool`
  refuse it. An image above 4 GiB (a position-independent test) has no 32-bit addresses to alias.
- `map_fixed` requests an address as a hint and checks it: Linux honours a free hint without `MAP_FIXED`, and the
  H700 kernels (4.9) lack `MAP_FIXED_NOREPLACE`.
- Order: `set_memory_map`, then `set_arena`, `map_fixed`, `set_stack_pool` (and `allocate_stack`). The locked
  cache is memory the port maps at 0xE0000000.
- Global `new` and `delete` route by thread: guest threads use the game's heap once the port installs it
  (`set_game_heap`), host code uses `malloc`, and `host_allocation_scope` marks host work on a guest thread. Before
  the game has a current heap (static construction), guest threads use `malloc`. Deletion routes by address. An
  exhausted game heap returns null to the game, as on the console.
- DCStoreRange and DCFlushRange report CPU writes the GX may read. DMA engines (disc, ARAM, card, THP, audio)
  call `before_dma_read` or `before_dma_write` first and report what they wrote with `report_cpu_write`. A cache
  store or flush is not a transfer: the game flushes memory holding valid copies too.
- Not provided natively: processor implementation registers (performance monitor, write-gather pipe, HID2) read
  their reset values; memory protection only traps stray writes on development hardware; the IPL ROM ANSI font
  keeps its geometry (24x24 cells, 512x512 I4 sheets) with blank glyphs, since games draw it only for disc errors,
  which cannot occur natively; floating-point exceptions the game enabled do not trap.

## Crash reports

Players send `log.txt` without access to the device, so reports must suffice on their own. Everything in
`starlet/os/crash_report.hpp` is async-signal-safe.

- `install_fatal_signal_report(program)` handles SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE and SIGABRT: signal,
  fault address, pc, lr, registers, backtrace and the executable's build ID, then ends the process with the
  signal so the launcher still sees it. Call once, early in `main`.
- Frames name `module+offset`, and for the executable the function from its own `.symtab` (the package keeps it
  and strips debug information).
- A library that catches the signal first (SDL restoring the console) restores this handler and raises the signal
  from its own handler. The report then walks past the signal frames to the original fault: a frame returning to
  the vDSO `__kernel_rt_sigreturn` (`mov x8, #139; svc #0`) is a signal handler. An asynchronous interrupt's SIGURG
  frame is no fault and is skipped.
- Crash reports name the guest thread running. The guest CPU thread has its own signal stack, so a guest stack
  overflow is reported. `brk #0x550b` is `-fsanitize-trap=return`: decompiled code with no return statement.
- `OSReport` text goes to a ring (`keep_game_message`), not the log; retail consoles discard it. A crash report
  ends with the last lines as ASCII (Shift-JIS bytes become `?`). `set_game_log(true)` also prints them.
- Profiles (`start_profiler`, env `<PREFIX>_PROFILE=<Hz>`, `_PROFILE_WALL`, `_PROFILE_THREADS`, `_PROFILE_FROM`,
  `_FRAME_STATS`) name samples on the guest CPU thread `guest<priority>`.

## Port startup

1. `starlet::os`: `set_memory_map`, `set_arena` (and the stack pool). `initialize_os` sets arena bounds, boot
   info and the calendar base before `main`.
2. Build the renderer and wrap it: `GlesBackend backend(gles_renderer)`.
3. `start_gpu(GpuSetup)`: `renderer`; `efb` (EFB height: RE4 and Pikmin 2 448, Galaxy 456; default 456);
   `start`/`stop` (run on the GPU thread: adopt the GL renderer, start its submission thread); `drawable_size`;
   `flush_dirty_state` (required); `diagnostics`. Call it before the game's `main` runs: `__GXPEInit` (from
   `GXInit`) talks to the GPU thread. It throws `std::invalid_argument` for an incomplete setup.
4. `set_video_presenter(starlet::gx_fifo::present)`.
5. Optional: `start_frame_trace()` before the game starts, `write_frame_trace(path)` at exit.

`flush_dirty_state` does what the SDK's `GXFlush` does with the decompilation's own state:
`if (__GXData->dirtyState) __GXSetDirtyState();`.

### What the decompilation provides or leaves out

- GXFifo.c and vi.c: leave out. Starlet provides:
  - GXFifo: `__GXFifoInit`, `__GXSaveCPUFifoAux`, `GXInitFifoBase`, `GXInitFifoPtrs`, `GXInitFifoLimits`,
    `GXSetCPUFifo`, `GXSetGPFifo`, `GXSaveCPUFifo`, `GXSaveGPFifo`, `GXGetCPUFifo`, `GXGetGPFifo`, `GXGetGPStatus`,
    `GXGetFifoStatus`, `GXGetFifoPtrs`, `GXGetFifoBase`, `GXGetFifoSize`, `GXGetFifoLimits`,
    `GXSetBreakPtCallback`, `GXEnableBreakPt`, `GXDisableBreakPt`, `GXSetCurrentGXThread`, `GXGetCurrentGXThread`.
  - VI: `VIInit`, `VIConfigure`, `VIFlush`, `VISetNextFrameBuffer`, `VIGetNextFrameBuffer`,
    `VIGetCurrentFrameBuffer`, `VISetBlack`, `VIWaitForRetrace`, `VIGetRetraceCount`, `VISetPreRetraceCallback`,
    `VISetPostRetraceCallback`, `VIGetNextField`, `VIGetCurrentLine`, `VIGetTvFormat`, `VIGetDTVStatus`. Retraces
    are progressive `VI_NTSC`.
  - Their other functions (`GXGetOverflowCount`, `GXRedirectWriteGatherPipe`, `__GXCleanGPFifo`, `__GXFifoLink`,
    `VIConfigurePan`, `VISet3D`, `VIGetScanMode`, ...) are not provided; a call fails to link.
- GXMisc.c: Starlet provides `GXFlush`, `GXResetWriteGatherPipe`, `GXAbortFrame`, `__GXAbort`, `GXReadDrawSync`,
  `GXSetDrawDone`, `GXWaitDrawDone`, `GXDrawDone`, `GXSetDrawSyncCallback`, `GXSetDrawDoneCallback`,
  `__GXPEInit`, `GXPeekARGB`, `GXPeekZ` and the `GXPoke*Mode` functions. Keep `GXSetMisc`, `GXSetDrawSync`,
  `GXPixModeSync`, `GXTexModeSync`, `GXCompressZ16`, `GXDecompressZ16` (they write through `GX_WRITE_*` and
  `__GXData`); patch the rest out.
- GXInit.c: keep; patch the hardware register pointers (`__piReg`, `__cpReg`, `__peReg`, `__memReg`) to RAM and
  the write-gather pipe setup (`PPCMtwpar`, HID2) out.
- GXDisplayList.c: `GXEndDisplayList` reads the PI wrap bit; take it from
  `GXGetFifoStatus(GXGetCPUFifo(), ..., &wrap)` after its `GXFlush` instead.
- GXPerf.c reads hardware counters: patch or leave out. GXFrameBuf.c compiles unchanged.
- The SDK's private FIFO struct (`__GXFifoObj`/`GXFifoObjPriv`) must keep the console's field order, as
  `starlet::sdk::GXFifoObjPriv` does.
- `GX_WRITE_*` and `GXWGFifo` come from `starlet/compat/gx_fifo.h`.

## Write-gather pipe and FIFO

Guest threads write commands through `starlet/compat/gx_fifo.h` (`starlet_gx_stage`, `starlet_gx_reserve`). A GPU
thread decodes them with `starlet::gx` and hands draws, copies and presentation to a `Renderer` in command order.
The game keeps its own command emitters (GXAttr, GXTev, GXGeometry, ...).

- Staged bytes go to the GPU thread in 64 KiB chunks while the CPU FIFO is linked to the GPU. Chunk buffers are
  reused. Writes reach the stage directly, so the pipe holds no partial burst to discard.
- An unlinked CPU FIFO takes writes in its own memory, big-endian, wrapping at its end as on the console (the PI
  wrap bit). A wrap overwrites commands the game has not yet run, as on the console. Pointing the GPU at it runs
  what lies between its read and write pointers.
- Status reads (`GXGetGPStatus`, `GXGetFifoStatus`) do not wait: games' GP-hang watchdogs call them from an alarm,
  and waiting there for the GPU thread would hold back every other interrupt (audio DMA, DSP mails). The GPU
  thread cannot hang and finishes queued work on its own, so aborts do not wait either. The FIFO never reaches
  its watermarks.
- The GPU never stops at a breakpoint: decoding runs in command order and draw-sync tokens raise their interrupts
  as they pass. A breakpoint without a callback (Super Mario Sunshine's `TDrawSyncManager` keeps the GPU one token
  behind with them) only changes how far the GPU may run ahead. `GXEnableBreakPt` with a callback set panics: its
  interrupt is not raised.
- `GXPokeARGB` and `GXPokeZ` are not provided; the poke modes are kept. A port that writes EFB pixels from the CPU
  calls `note_efb_poke()` after each write.

### Vertex array byte order

Arrays hold canonical (big-endian) GX bytes. Arrays the game fills with native numbers (shadow geometry, matrix and
light arrays) are registered with `starlet_gx_host_order_array` and released before reuse; registrations travel with
the command stream, so draws before and after see the array as the game meant at that point. A port that converts
the vertex data it loads to host order (Super Mario Sunshine's resource converters) sets
`GpuSetup::arrays_host_order`: every array is then read as host-order numbers at its own width, and retail bytes it
leaves big-endian are registered with `starlet_gx_big_endian_array`. Textures, TLUTs and display lists stay
canonical either way.

### Source change detection

Console code must flush the data cache (`DCFlushRange`, `DCStoreRange`) before the GPU reads what the CPU wrote,
and every DMA into memory reports its range. The GPU thread keeps a write generation per 4 KiB arena page and
hashes a texture's source only when its pages changed (and every 16th frame regardless). Host-order arrays have
generation 0 (unknown): native code writes them without reporting. Display lists are parsed in place, so their
pages' generation names their bytes.

## Draw sync

The pixel engine's interrupts are raised on the GPU thread in command order: BP 0x45 draw done, BP 0x47/0x48
token. A draw the renderer refuses (it throws) is reported once per message and skipped: the console's GPU would
draw something and the stream after it stays valid. `gpu_failure()` names the first command stream failure.

A command the decoder refuses (garbage in a display list the game freed and reused, for one) is reported and the
decoder recovers (`Decoder::recover`): the registers keep their values, as the console's GPU keeps them whatever it
reads, since the game never sends again the state it believes the GPU holds. A failed display list ends there and
the stream goes on after its call. A failure with no command boundary after it drops the rest of its submission;
the draw-done write that ends a `GXSetDrawDone` submission still runs, so the game waiting for it goes on. A failed
call on the GPU thread (a peek outside the EFB) is reported and leaves the stream alone.

`GXWaitDrawDone` returns once the GPU has passed the newest `GXSetDrawDone`. The SDK's flag rises at any draw-done
interrupt; on the console the GPU reaches a draw done long before the game sets the next one, so the flag means
its own. JFWDisplay sets one without waiting after its display copy and waits for another at the end of the frame,
and the GPU thread can fall frames behind the game (GL stalls): an older draw done would end that wait, the game
would run frames ahead of the GPU, and free and reuse memory the GPU had yet to read (The Wind Waker's actors at a
stage change). Each draw-done interrupt still calls the game's callback.

A token whose callback read the EFB carries an EFB snapshot taken as it executed; it and later interrupts are
raised once that snapshot is read back, and the guest releases it. Other tokens carry no snapshot. See below.

## VI and frame flow

A 59.94 Hz retrace interrupt latches the frame buffer registers, then calls the presenter on the guest CPU
(`set_video_presenter`). The presenter (`starlet_gx_fifo::present`) queues presentation behind the commands
written so far and must not block. Without a presenter, retraces show nothing. A display copy ends the frame
for peek bookkeeping; `VIFlush` ends it for guest-side peek groups. `VISetNextFrameBuffer` names the XFB by its
pointer.

`GpuSetup::frame_ahead` (or `STARLET_GX_FRAME_AHEAD=1`) lets the renderer queue a second frame for the GPU before
the first completes (`Renderer::set_frame_ahead`; the GLES backend keeps two frames in flight). The GL thread
waits less on the GPU, and the picture lags the game's input by one frame more, so it is off by default. Draw
done, tokens, peeks that read the EFB and DMA over resident copies still wait for every command before them.

## EFB peeks

`GXPeekARGB` and `GXPeekZ` read the EFB at their point in the command stream: top-left origin, D24 depth, colour
as `0xAARRGGBB`. By default (`GpuSetup::sync_reads` off) the game does not wait for the GPU there:

- Peeks with no command between them form a group. Groups are numbered in the order they occur in a frame;
  `VIFlush` ends a frame.
- A group's first peek has the GPU thread snapshot the EFB at that point (`Renderer::snapshot_efb_exact`). The
  group's peeks are answered from the snapshot of the same group one frame earlier, when that group began at the
  same call site.
- A group seen for the first time, a group missing from the previous frame, more than four groups in a frame,
  and every group after `note_efb_poke` in that frame read the EFB as of their own point, waiting for the GPU
  thread.
- A snapshot covers the pixels its group read one frame earlier, widened by 32 pixels. A peek outside it, or
  before its readback arrived, reads that snapshot through the GPU thread.

Draw-sync callbacks that read the EFB (`GXSetDrawSyncCallback`) read the snapshot their token took one frame
earlier, by token value and its ordinal in the frame; display copies end these frames. The first callback reads the
live EFB and teaches the GPU thread to snapshot later tokens of that value. The next one (and the first after a
frame without that token or after `note_efb_poke`) reads its own token's snapshot, and its interrupt waits for that
readback. Every token of such a value takes a snapshot for the next frame; snapshots of tokens that did not recur
are released at the display copy. The snapshot region is the rectangle the callback read last time, widened by 32
pixels; the whole EFB before it read a pixel.

`GpuSetup::peek_lag` (`STARLET_GX_PEEK_LAG=N`, 1 to 3) answers a callback whose snapshot is not read back yet from
the newest older snapshot of its token that is, instead of the GPU thread waiting; see `gpu.hpp` for the trade-off.

`sync_reads` on, or `STARLET_GX_SYNC_READS=1`, makes every peek exact: the peek waits for the GPU thread, and a
draw-sync callback's interrupt waits for its token's snapshot. It overrides `peek_lag`. The stats line counts peek
groups and draw-sync callbacks answered one frame late as `late`.

`request_depth_readback` gives the EFB's depth as of the commands written so far without waiting for the GPU to
execute them (`-1` when the backend only reads live).

## Copies the CPU reads

A port names memory whose texture copies the game reads on the CPU with `Renderer::write_back` (the GLES backend
reads such copies back without waiting). The GPU thread writes each copy's bytes into memory after the command
during which its readback arrived, in issue order, and before the next copy into the same memory, so the CPU sees
a copy up to about a frame late. Draw-done and token interrupts do not wait for them. On the console an interrupt
follows the copy's memory writes, but holding it until the GPU had finished the copy made the game wait for the
GPU in every frame.

## Host transfers

`start_gpu` observes `starlet::os::before_dma_read` and `before_dma_write`. A transfer whose range may hold results
the renderer keeps off memory (`Renderer::resident_in`) waits while the GPU thread runs the commands queued before
it and then the renderer's `before_dma_read` or `before_dma_write`: a read gets those results, a write drops those
it replaces. A read waits for the GL thread's readback too. Other transfers do not wait. Observers may block, so
callers hold no lock the GPU or GL threads take. Calls handed to the GPU thread are allocated from the host: a
guest thread's game heap may be gone at a transfer (Resident Evil 4 transfers its heap back from ARAM before
reinstating it). Command buffers the GPU thread has run are recycled for the guest's next ones, because a fresh
buffer costs the guest an allocation and a clear.

## Diagnostics

`GpuSetup::diagnostics`: `stats` (renderer and decoder counters every 300 retraces), `efb_dumps` (EFB as
`<capture_dir>/efb-<retrace>.ppm`; needs the renderer's GL work on the GPU thread), `trace_draws`/`trace_from`
(draw state on stdout). Refused draws are logged once per message so a frequent failure cannot hide others.

## Tests

`tests/os` and `tests/gx_fifo` (host, Linux) cover threads and priorities, interrupts with and without
asynchronous delivery, crash reports, game memory aliasing, FIFO staging and wrap, token and draw-done order, peek
groups and late answers, draw-sync snapshots, peek lag, frame ahead, host-order arrays, and (with GLES headers)
DMA and write-back over resident copies on a recording GL fake.
