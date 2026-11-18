# Replay traces

A GX trace is a few frames of one game's GX work: every decoder call, the sink callbacks and renderer
calls they caused, and every byte of game memory the decoder and renderer read. `starlet-replay` runs a
trace through the decoder and the GLES renderer offscreen on Mesa llvmpipe and hashes each presented
frame. `tools/replay-all.sh` replays every trace against the checkout and compares the frames with
references made by the replayer at a known-good commit, so shared-path changes are checked against
every port without a device.

Traces hold game data. Keep them in `starlet-traces/` beside the Starlet checkout and never commit them.

## Gate for decoder and renderer changes

Before pushing a change to the decoder or the renderer, run `tools/replay-all.sh` on the change rebased
onto origin/master: the shader check and every trace must PASS. A trace that changes on purpose gets
new references from the change itself (`--update`) after its diff images were checked, in the same
push. A full run takes a few minutes on Apple silicon; `--shaders` alone takes seconds.

## Check a change

```sh
tools/replay-all.sh            # shader check, then PASS, DIFF, DIVERGED or FAILED per trace
tools/replay-all.sh --shaders  # the shader check alone, in seconds
tools/replay-all.sh --twice    # also replays each trace twice and compares the runs
tools/replay-all.sh --only galaxy-garden
```

The shader check (`starlet-replay --shaders`) compiles and links with Mesa every program the
renderer generates outside draws, on both shading paths: texture copies over FORMAT, HALF, RGBA6,
LUMINANCE, DEPTH, FILTER and SPLIT, display copies, presentation, clears and format changes, bloom,
snapshots, offscreen passes and write-backs. It also builds every program in the program lists of
the references (`<trace>.ref/programs.list`, fast shading) and in `starlet-traces/programs/*.list`
(the lists the ports ship). Draw programs of every trace compile during its replay; a program the
driver rejects fails the trace.

It builds the replayer in the `starlet-replay:bookworm` container (`tools/replay/Dockerfile`, native
aarch64 on Apple silicon). A DIFF names the frames that changed and writes, per frame, reference |
actual | amplified difference to `$STARLET_REPLAY_OUT/<trace>/diff-frame-NNN.png`. DIVERGED means the
decoder made different sink callbacks than the recording (a decoder change); FAILED means the renderer
failed, the driver rejected a generated program, or the trace is unreadable.

## Record a trace

1. Build the port against your Starlet checkout with `-DSTARLET_GX_TRACE=ON`. Without the option the
   recorder is not compiled and the build is unchanged.
2. Run the game with these variables:

   | Variable | Meaning |
   | --- | --- |
   | `STARLET_GX_TRACE` | Output file, on the device's card rather than `/tmp` (traces run to hundreds of MB). |
   | `STARLET_GX_TRACE_START` | Frame boundaries (presentations) to start at, comma-separated; default 0. |
   | `STARLET_GX_TRACE_TRIGGER` | Instead: start a recording each time this file appears; the recorder removes it. |
   | `STARLET_GX_TRACE_FRAMES` | Frames per recording; default 8. |
   | `STARLET_GX_TRACE_NAME` | Port and scene, shown by the replayer. |

   Several recordings in one run are numbered: `gx.trace` becomes `gx-1.trace`, `gx-2.trace`.
   Recording reports every read as unchanged-unknown (generation 0), so the renderer reads and hashes
   everything it uses; the game runs slower while it records.
3. Compress it into the trace directory: `zstd -19 gx-1.trace -o starlet-traces/<port>-<scene>.trace.zst`.
4. Make its reference at a known-good commit (below) and check it twice: `tools/replay-all.sh --twice --only <name>`.

Pick scenes for GX coverage: EFB copies, offscreen passes, indirect textures, fog, Z textures, CI/TLUT
textures, deflicker and half-scale copies, high draw counts.

## Update references

References are the replayer's own frames, not device captures: llvmpipe and Mali round differently.
Update them only from a commit whose output you accept:

```sh
git switch --detach <known-good commit>
tools/replay-all.sh --update [--only NAME]
```

`--update` writes `<trace>.ref/hashes.txt` (headed by the commit), the frame PNGs and, for fast
shading, the trace's `programs.list`. A change that
alters output on purpose updates the references in the same step, after checking its diff images.

## What a replay does not cover

- The port's own code, its sink logic and its threads: the trace replays the calls they made.
- Write generations and source identities: they replay as unknown, so caches validate by content.
- Timing: write-backs land waiting, programs build before their draws, and frames run in trace order.
- Work before the recording: a frame that presents an XFB copied earlier shows its stale memory.
- GPU differences: Mali and Panfrost rounding, driver bugs and performance.
