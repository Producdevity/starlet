# GX FIFO and VI

`starlet::gx_fifo` (`starlet_gx_fifo`, Linux) implements the GX SDK calls that
drive hardware (the write-gather pipe, FIFO objects, synchronization, EFB peeks)
and the VI, for SDK-level ports. Guest threads write commands through
`starlet/compat/gx_fifo.h`; a GPU thread decodes them with `starlet::gx` and
hands draws, copies and presentation to a `Renderer` in command order. The game
keeps its own command emitters (GXAttr, GXTev, GXGeometry, ...).

Declarations: `starlet/sdk/gx.h`, `starlet/sdk/vi.h`. Port API:
`starlet/gx_fifo/gpu.hpp`, `video.hpp`, `renderer.hpp`, `gles_renderer.hpp`,
`frame_trace.hpp`.

## Port startup

1. `starlet::os`: `set_memory_map`, `set_arena` (and the stack pool). GX
   addresses are physical addresses in that window; copy destinations and TLUT
   loads must lie in its first 64 MiB.
2. Build the renderer and wrap it: `GlesBackend backend(gles_renderer)`.
3. `start_gpu(GpuSetup)`: `renderer`, `efb` (EFB height: RE4 and Pikmin 2 448,
   Galaxy 456), `start`/`stop` (run on the GPU thread: adopt the GL renderer,
   start its submission thread), `drawable_size`, `flush_dirty_state` (required,
   see GXFlush below), `diagnostics`. Call it before the game's `main` runs:
   `__GXPEInit` (from GXInit) talks to the GPU thread.
4. `set_video_presenter(starlet::gx_fifo::present)`.
5. Optional: `start_frame_trace()` before the game starts, `write_frame_trace(path)`
   at exit.

`flush_dirty_state` is a port function doing what the SDK's GXFlush does with
the decompilation's own state: `if (__GXData->dirtyState) __GXSetDirtyState();`.

## Functions provided

GXFifo.c: `__GXFifoInit`, `__GXSaveCPUFifoAux`, `GXInitFifoBase`,
`GXInitFifoPtrs`, `GXInitFifoLimits`, `GXSetCPUFifo`, `GXSetGPFifo`,
`GXSaveCPUFifo`, `GXSaveGPFifo`, `GXGetCPUFifo`, `GXGetGPFifo`, `GXGetGPStatus`,
`GXGetFifoStatus`, `GXGetFifoPtrs`, `GXGetFifoBase`, `GXGetFifoSize`,
`GXGetFifoLimits`, `GXSetBreakPtCallback`, `GXEnableBreakPt` (the GPU does not
stop there; panics when a breakpoint callback is set, whose interrupt is not raised),
`GXDisableBreakPt`, `GXSetCurrentGXThread`, `GXGetCurrentGXThread`.

GXMisc.c: `GXFlush`, `GXResetWriteGatherPipe`, `GXAbortFrame`, `__GXAbort`,
`GXReadDrawSync`, `GXSetDrawDone`, `GXWaitDrawDone`, `GXDrawDone`,
`GXSetDrawSyncCallback`, `GXSetDrawDoneCallback`, `__GXPEInit`, `GXPeekARGB`,
`GXPeekZ`, `GXPokeAlphaMode`, `GXPokeAlphaRead`, `GXPokeAlphaUpdate`,
`GXPokeBlendMode`, `GXPokeColorUpdate`, `GXPokeDither`, `GXPokeDstAlpha`,
`GXPokeZMode` (the poke modes are kept; `GXPokeARGB` and `GXPokeZ` are not
provided).

vi.c: `VIInit`, `VIConfigure`, `VIFlush`, `VISetNextFrameBuffer`,
`VIGetNextFrameBuffer`, `VIGetCurrentFrameBuffer`, `VISetBlack`,
`VIWaitForRetrace`, `VIGetRetraceCount`, `VISetPreRetraceCallback`,
`VISetPostRetraceCallback`, `VIGetNextField`, `VIGetCurrentLine`,
`VIGetTvFormat`, `VIGetDTVStatus`. Retraces run at 60000/1001 Hz, progressive,
`VI_NTSC`.

Write-gather pipe (`starlet/compat/gx_fifo.h`): `starlet_gx_stage`,
`starlet_gx_reserve`, `starlet_gx_host_order_array`, `starlet_gx_big_endian_array`,
`starlet_gx_release_array`.

Vertex array byte order: arrays hold canonical GX bytes, and arrays the game fills
natively are registered with `starlet_gx_host_order_array`. A port that converts
the vertex data it loads to host order sets `GpuSetup::arrays_host_order`: every
array is then read as host-order numbers at their own width (bytes as stored),
and retail bytes it leaves big-endian are registered with
`starlet_gx_big_endian_array`. Textures, TLUTs and display lists stay canonical
either way.

## The port's decompilation

- GXFifo.c and vi.c: leave out. Their other functions (`GXGetOverflowCount`,
  `GXRedirectWriteGatherPipe`, `__GXCleanGPFifo`, `__GXFifoLink`,
  `VIConfigurePan`, `VISet3D`, `VIGetScanMode`, ...) are not provided; a call
  fails to link.
- GXMisc.c: keep `GXSetMisc`, `GXSetDrawSync`, `GXPixModeSync`,
  `GXTexModeSync`, `GXCompressZ16`, `GXDecompressZ16` (they write through
  `GX_WRITE_*` and `__GXData`); patch the rest out.
- GXInit.c: keep; patch the hardware register pointers (`__piReg`, `__cpReg`,
  `__peReg`, `__memReg`) to RAM and the write-gather pipe setup (`PPCMtwpar`,
  HID2) out.
- GXDisplayList.c: `GXEndDisplayList` reads the PI wrap bit; take it from
  `GXGetFifoStatus(GXGetCPUFifo(), ..., &wrap)` after its `GXFlush` instead.
- GXPerf.c reads hardware counters: patch or leave out.
- GXFrameBuf.c compiles unchanged.
- The SDK's private FIFO struct (`__GXFifoObj`/`GXFifoObjPriv`) must keep the
  console's field order, as `starlet::sdk::GXFifoObjPriv` does.
- `GX_WRITE_*` and `GXWGFifo` come from `starlet/compat/gx_fifo.h`. Arrays the
  game fills with native 32-bit numbers for `GXSetArray` are registered with
  `starlet_gx_host_order_array` and released before reuse.

## Diagnostics

`GpuSetup::diagnostics`: `stats` (renderer and decoder counters every 300
retraces), `efb_dumps` (EFB as `<capture_dir>/efb-<retrace>.ppm`; needs the
renderer's GL work on the GPU thread), `trace_draws`/`trace_from` (draw state
on stdout). `gpu_failure()` names the first command stream failure; refused
draws are logged once per message.

## Tests

`tests/gx_fifo` (host, Linux): staging across chunks, unlinked FIFOs and wrap,
token and draw-done order, peeks, a display copy presented by VI.
