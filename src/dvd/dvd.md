# starlet::dvd

The DVD SDK calls over the discs `starlet::disc` installed, for SDK-level ports (`include/starlet/sdk/dvd.h`).
Moved from the Pikmin 2 port.

- `initialize` takes each disc's `sys/` and the file roots its FST paths resolve in, in order: a later
  disc usually lists its own differing files, then the first disc's tree (`install_discs`).
- Entry numbers, offsets and lengths are each disc's own; paths resolve with the SDK's rules
  (`FileTable::resolve`). The first disc's FST is copied below the arena's top, as the apploader
  leaves it, for `DVDGetFSTLocation`.
- Reads run in order on one drive thread and complete as interrupts on the guest CPU. The drive thread
  calls `before_dma_write` before reading into memory; completion reports the bytes (`report_cpu_write`). Status polls
  that find the drive busy wait for the next interrupt, as the console's CPU took it inside the loop.
- `DVDChangeDiskAsync` switches to the requested installed disc when the change completes; the player
  never swaps discs.
