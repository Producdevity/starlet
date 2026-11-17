# SDK services

SDK-level ports link these modules instead of the console SDK's units. Each one defines the SDK's C
symbols (C linkage, bound by name) over the layouts in `starlet/sdk/*.h`. Threads, interrupts and
the guest CPU are in [OS-and-GX-FIFO](OS-and-GX-FIFO.md); the audio device, controllers and quit
are in [Host-services](Host-services.md).

## AI (audio DMA)

`starlet::ai` knows nothing of AX or JAudio. The game, or `starlet::ax`, programs blocks and handles
the interrupt. The model comes from the Pikmin 2 port.

- `AIInitDMA` (guest address) and `starlet::ai::init_dma` (host pointer) copy the block when
  programmed, after `before_dma_read`. A block is native s16 stereo, right channel first, 32-byte
  aligned, a multiple of 32 bytes (at most 0x7FFF * 32). Anything else aborts.
- `AIStartDMA` plays the programmed block. When it finishes, the DMA latches the block programmed
  after it and raises the AI interrupt on the guest CPU, which calls the DMA callback. The game then
  programs the next block.
- A block that ends with nothing newly programmed waits up to 12 ms, then the DMA replays the last
  block, as the hardware does when software is late. `stats()` counts completions, waits, repeats
  and the wait time.
- `AIInit` opens `starlet::host::AudioOutput` in pull mode. Selector 0 plays at 32028 Hz, 1 at
  48043 Hz (the console's 32028.5 and 48043 Hz, rounded). The output thread pulls 5 ms slices on a
  steady clock; the DMA swaps the channels for the device. Without a device a silent clock pulls
  at the rate and `device_error()` says why. Volume, mute and mixer settings stay as the firmware
  has them.
- `AISetDSPSampleRate` reopens the output at the new rate; it aborts if the DMA runs.
- `set_capture(path)`, before `AIInit`, records what the DMA plays to a 16-bit stereo WAV.
- `AIReset` clears the init flag, as the SDK does; the DMA and the output keep running.
- Not modelled: disc audio streaming. The stream volume and play-state registers keep their values;
  starting a stream logs one line and the stream stays silent. `AIRegisterStreamCallback`, the
  stream sample counter, trigger and stream rate are absent: no current port calls them.

## AX (voice library)

`starlet::ax` implements the SDK's AX voice library: the CPU side the SDK compiles (voice stacks,
parameter-block setters, the per-frame sync) and the DSP microcode's 5 ms frame. It plays through
`starlet::ai`, as the console's AXOut does through the AI DMA. First user: the Resident Evil 4
port. Call `AIInit` before `AXInit`.

### What the port compiles

| Keep compiling | Starlet defines |
| --- | --- |
| `syn*.c`, `seq.c`, `axart*.c`, `mix.c`, `axfx.c`, `reverb_*.c`, `chorus.c`, `delay.c`, `ax_rna.c` | `AX.c`, `AXAlloc.c`, `AXAux.c`, `AXCL.c`, `AXOut.c`, `AXProf.c`, `AXSPB.c`, `AXVPB.c`, `DSPCode.c`, `ai.c` (`starlet::ai`) |
| `AXComp.c`: the compressor's gain table `__AXCompressorTable`, which Starlet links against | |

SYN, SEQ, AXART, MIX and AXFX are CPU code (envelopes, pitch, pans, effects, parameter-block
writes) and run unchanged. The AX units are the DSP's half (`DSPCode.c` is the microcode image,
`AXOut.c` drives the DSP mailbox and the AI DMA) and the code that hands parameter blocks to it.
Their state is shared with the DSP model, so Starlet owns all of them. `AXComp.c` is data the
microcode reads; the port links it.

The SDK's `__AX*` internals and `axDspSlave` are not defined; nothing outside the AX units uses
them. `include/starlet/sdk/ax.h` declares the types with the SDK's field order and native pointers.
A port asserts its decompilation's `AXVPB`, `AXPB` and `AX_AUX_DATA` against them (see
[Compat and SDK headers](#compat-and-sdk-headers)).

### Frames

The game calls `AIInit`, then `AXInit` or `AXInitEx(mode)`. `AXInit` registers an AI DMA callback,
programs a silent buffer and starts the DMA. The silent first block plays twice, as on the console.
Each AI interrupt, on the guest CPU, is the SDK's `__AXOutAiCallback`:

1. Sync: copy what the game changed in each voice's block into the DSP's copy, by sync flag, and
   copy the DSP's state, address and envelope volume back. Voices are visited by priority, 31
   first. A voice that pushes the SDK's cycle estimate past `AXGetMaxDspCycles` (405000 by default)
   stops, and its drop callback runs in step 3. `AXGetDspCycles` returns the estimate.
2. The DSP frame (`dsp.cpp`) runs with the command list built in the previous interrupt and mixes
   into one of three output buffers.
3. Drop callbacks, aux callbacks, the game's frame callback, then the next command list (mode, aux
   buses with a callback, compressor).
4. Mode 1 (triple buffering, RE4's) programs the buffer mixed one interrupt earlier; mode 0
   programs the buffer just mixed.

The DSP finishes within the interrupt, so the SDK's late-DSP path (`lessDspCycles`) is always 0.
`starlet::ai` paces the interrupts at the device's rate and replays a block when the guest is late.

### The DSP frame

After Dolphin's AX HLE (GPL-2.0-or-later, `Source/Core/Core/HW/DSPHLE/UCodes/AX.cpp`, `AXVoice.h`,
`AXStructs.h`) and its accelerator (`Source/Core/Core/DSP/DSPAccelerator.cpp`). Dolphin is the
reference; behaviour is not verified against hardware.

- Setup: each of the nine buses (main, aux A, aux B; L, R, S) starts at the depop value of dropped
  voices and steps toward zero by at most 20 a sample (`AXSPB.c`).
- Stereo mode adds the previous frame's S bus to L and R; surround mode adds it to R and subtracts
  it from L.
- Voices, in block order, each millisecond (32 samples): apply the block's per-millisecond updates;
  for a running voice read samples through the accelerator and converter, apply the envelope (`ve`:
  signed volume, per-sample delta) and the one-pole low-pass (`lpf`), then add to each bus enabled
  in `mixerCtrl` with its volume and optional per-sample ramp. Each bus contribution saturates to
  16 bits. The last sample per bus is kept as the voice's depop value.
- Accelerator: ADPCM (4-bit, 8-byte frames with a predictor/scale header, nibble addresses), PCM16
  and PCM8 (sample addresses, big-endian in ARAM, gain 0x800 and 0x100 set by `AXSetVoiceAddr`).
  The wrap compares the current address with the end address for equality, so a current address
  beyond the end keeps reading memory (counted in the diagnostics). Reading the sample at the end
  address loops to the loop address with the loop predictor/scale and, unless the voice is a
  stream, the loop history. Without the loop flag the voice stops. An ADPCM end address at a
  frame's first or second nibble wraps without reading a header.
- Converter: 16.16 ratio up to 4.0. Type 0 is 4-tap polyphase with 128 phases, type 1 linear, 2
  none. Both interpolating types use a four-sample history, oldest first, as Dolphin.
- Aux: with a callback registered, the aux bus goes to the CPU's buffer and the buffer the callback
  processed a frame earlier returns into main L/R/S, two frames after the send (`AXAux.c`'s three
  buffers). Without a callback the bus is dropped. DPL2 mode (2) sends aux A L/R/S and aux B S to
  the aux A callback (`AX_AUX_DATA_DPL2`), returns them to main L/R and aux B L/R, then loops aux B
  L/R (the rear channels) back into main L/R one frame later.
- Compressor (on by default, `AXSetCompressor`): when main L or R exceeds 32768, the frame is scaled
  by an attack frame of `__AXCompressorTable` (11 attack frames, then 10 release frames; 21 frames
  of 160 gains).
- Output: L and R saturate to 16 bits, right channel first in the buffer as the AI DMA reads it. S
  is kept for the next frame.

### Differences from Dolphin

- Dolphin assigns the previous S bus to the main buses in stereo and surround mode, which discards
  the main buses' depop ramps. Starlet adds it and keeps them.
- The SRC coefficient tables (`coefSelect` 0-2 for 8, 12 and 16 kHz, 128 phases of 4 taps) are
  computed on first use with the windowed-sinc formula of Dolphin's free DSP ROM
  (`docs/DSP/free_dsp_rom/generate_coefs.py`). The console's ROM tables differ slightly.

### Not modelled

- Initial time delay (ITD). The SDK fields are synced; the DSP mixes without the delay, as Dolphin.
  A voice with a nonzero shift logs one line. RE4 enables ITD only through AXART sounds with zero
  targets.
- FIR (`AXSetVoiceFir`): stored and synced; no AX microcode uses it.
- `COPYALL` sync copies the game's update words to the DSP. The SDK copies them the other way,
  which loses them; nothing in RE4 uses it.
- A voice format with an invalid gain scale or sample size logs one line (an invalid sample size
  reads as 0). `AXSetStepMode` does nothing: single-stepping is a debug-build DSP feature.
- Accelerator decodes 1 and 3 read the accelerator's input register, which AX never writes; they
  read 0.

### Diagnostics

Always on (`starlet/ax/diagnostics.hpp`). Every line goes to stderr with the prefix
`[audio] t=<seconds since start>`.

- Voice checks run once per 5 ms frame on each running voice after the sync, so they cover game
  writes and update-queue changes. A line prints when a voice starts or its findings change: ratio
  0 or above 2.0, polyphase or invalid SRC type, loop/end/current address outside ARAM, ADPCM
  predictor word above 0x7f or scale above 12, a looped ADPCM voice with an all-zero loop context,
  LPF on, ITD on. Findings in the first group print at most 8 lines per second and 300 per run;
  polyphase, LPF and ITD notes at most 3 per second and 100 per run. Dropped lines are counted in
  the next window's first line.
- Once per second, when non-zero: output samples clamped in the main mix, aux A and aux B return
  samples beyond 16 bits (after the callbacks), and 1 ms voice blocks whose current address is
  past the end address.
- A ring holds the last 10 s of the main mix and the aux A and B returns (the mean of L and R),
  2.56 MB. `save_audio_clip` writes `audio-clip-<seconds>.wav` (32 kHz, 4 channels: main L, main
  R, aux A, aux B) and `.txt` into the folder given to `set_diagnostics_directory`: the voices
  that started in the window with their parameter-block words and the run's totals. Ten trigger
  clips per run; the `exit` reason always writes `audio-clip-exit.*`. `request_audio_clip` does the
  same on a worker thread; a request while one runs is dropped.
- `AXQuit` then `AXInit` between scenes keeps the clip window and the directory.

## DVD

`starlet::dvd` serves the DVD SDK calls over the discs `starlet::disc` installed. The model comes
from the Pikmin 2 port.

- `initialize` takes each disc's `sys/` (`boot.bin`, `fst.bin`) and the file roots its FST paths
  resolve in, in order. A later disc usually lists only its differing files, then the first disc's
  tree (`install_discs`).
- Entry numbers, offsets and lengths are each disc's own. Paths resolve with the SDK's rules
  (`FileTable::resolve`): every `/` restarts at the root, as games build `dir//full/path` names.
- The first disc's FST is copied below the arena's top, sized for the largest table, as the
  apploader leaves it (`DVDGetFSTLocation`).
- Reads run in order on one drive thread and complete as interrupts on the guest CPU. The drive
  thread calls `before_dma_write` before reading into memory; completion reports the bytes
  (`report_cpu_write`). Bytes past the end of a file read as zero and count in the result.
- The drive stays busy until the completion interrupt has run, so a caller that polls the status
  until idle finds its block final and its callback done. Status polls that find the drive busy
  wait for the next interrupt, as the console's CPU took it inside the loop; other interrupts (an
  ARAM transfer the same loop waits for) are delivered there too.
- `DVDChangeDiskAsync` switches to the requested installed disc when the change completes; the
  player never swaps discs.

## PAD

`PADRead` returns the controller states the port publishes from its input sampling
(`starlet::host::to_gamecube`), already within the console's calibrated ranges (about +-72 main
stick, +-59 C stick, triggers 0-255) with the dead zone applied by the sampler. `pad::publish`
is called from a host thread; `PADRead` runs on the guest and copies the latest state under a
lock. `PADClamp` and `PADClampCircle` do nothing for that reason. Rumble, calibration and sampling
rate calls succeed without effect. See [Host-services](Host-services.md) for controllers.

## ARAM

`starlet::ar` holds 16 MiB of ARAM and its DMA (AR and ARQ). Transfers copy when posted and
complete as interrupts: one timing the hardware's queued DMA can produce, so `ARQFlushQueue` has
nothing to flush. Main-memory addresses resolve through `starlet::os::hardware_to_native`; each
transfer calls `before_dma_read` (main to ARAM) or `before_dma_write` (ARAM to main) before
copying. A main address outside game memory panics; a transfer past the end of ARAM aborts.
`starlet::ar::memory()` exposes ARAM for the AX DSP model, which reads samples from it directly.
The model comes from the Pikmin 2 port.

## NAND

`starlet::nand` serves the Wii SDK's NAND calls (`starlet/sdk/nand.h`) from a save folder. The
model comes from the Super Mario Galaxy port's platform layer.

- `start(directory, hooks)` takes an existing folder the port owns and creates `data/` and `tmp/`
  in it. The game's paths resolve inside it and never reach the firmware's storage: relative paths
  and `/home/...` (`NANDGetHomeDir` returns `/home`) go to `data/`, `/tmp/...` to `tmp/`,
  `/shared2/...` to the folder itself (a missing Mii database reads as absent). Other absolute
  paths return `NAND_RESULT_ACCESS`.
- Paths are bounded to `NAND_MAX_PATH` and eight components. Empty, `.` and `..` components and
  symbolic links anywhere along the path are refused. Files are private to the process owner;
  nonzero NAND attributes are refused.
- Create keeps an existing file. Reads return the bytes read, 0 at the end. Close syncs a writer.
  Move renames a file into a directory and syncs both directories, so a game that writes to `/tmp`
  and moves the file home replaces its save atomically.
- Sixteen handles, each tied to its exact `NANDFileInfo`: a caller's integer cannot close a
  descriptor it does not own. Files are bounded to 16 MiB.
- `NANDCheck` compares the request with the host file system's free space and inodes, not the
  console's quotas.
- Read-safe opens work. A write-safe open returns `NAND_RESULT_ACCESS` (there is no copy-and-commit
  transaction), never an ordinary write in its place.
- Asynchronous calls run on one worker in submission order, from 32 fixed request slots. A command
  block that is still pending cannot be submitted again (`NAND_RESULT_BUSY`); a callback may reuse
  its own block. The callback runs on the worker after the request's slot is free, through
  `Hooks::complete` when the port gives one: a port whose callbacks are interrupts disables its
  interrupts around it. Callers keep buffers and blocks alive until completion.
- `Hooks::enter` and `leave` bracket every section that holds the service's lock, for ports whose
  threads can be cancelled (defer cancellation there). Both or neither.
- `stop()` refuses new work, runs the queued requests and their callbacks, joins the worker and
  closes the remaining handles. Call it with no other thread inside a NAND call, never from a
  completion.

## THP

`starlet::thp` decodes THP movies from bytes as read from the disc (big-endian). The model comes
from the Super Mario Galaxy port's platform layer. Malformed input throws `starlet::thp::Error`.

- `VideoDecoder::decode`: one frame of baseline JPEG with 4:2:0 sampling into the GX I8 Y, U and V
  tiles `THPDraw` samples (8x4 texels, row-major tile order; planes cover whole MCU rows, and the
  tiled planes start with the visible rows). As in the SDK's `THPVideoDecode`, the entropy-coded
  data has no 0xFF00 byte stuffing and no restart markers: a restart interval only realigns the bit
  stream to a byte and resets the DC predictions. Huffman decoding follows ITU-T T.81 F.2.2.3 with
  a 9-bit lookup; the inverse DCT is the AAN float algorithm, within one sample step of FFmpeg's
  THP decoder. Blocks go into decoder scratch and reach the planes only once the frame has decoded
  completely. Packet, dimension and destination bounds are checked. `frame_size` reads a frame's
  dimensions from its start-of-frame segment.
- `read_header`, `read_components`, `read_video_info`, `read_audio_info`, `read_frame_sizes`: the
  movie's metadata in the SDK's layouts (`starlet/sdk/thp.h`), checked against the file's size, the
  decoders' limits and each frame's extent.
- `decode_audio`: one record (a 0x50-byte header with the right channel's offset, samples per
  channel, both channels' predictor coefficients and history, then 8-byte ADPCM frames of a
  predictor/scale byte and 14 four-bit samples) with the SDK's `THPAudioDecode` arithmetic: 64-bit,
  rounded at bit 16, clamped to 32 bits. Output is right then left, interleaved or planar; mono
  data fills both.
- A port with its own player links `starlet::thp`. `starlet::thp_sdk` adds the SDK's `THPInit`,
  `THPVideoDecode` and `THPAudioDecode` for ports that keep the SDK's player (Linux, on
  `starlet::os`). `THPVideoDecode` gets no frame length, as in the SDK: the frame ends with its
  scan, bounded at 4 MiB. It reports the planes it writes with `before_dma_write` and
  `report_cpu_write`, because the SDK's locked-cache DMA bypassed the data cache and GX texture
  caches must see the new frame.

## Compat and SDK headers

`starlet/sdk/*.h` declare the SDK's types and functions with the console SDK's field order and
values, native pointers and C linkage, so a game's calls bind by name. A port's decompilation
declares the same types; it includes Starlet's header first with `STARLET_SDK_TYPES_ONLY` and
asserts at compile time that its own `AXVPB`, `AXPB`, `AX_AUX_DATA`, `NANDFileInfo`,
`NANDCommandBlock`, `NANDBanner`, `CARDFileInfo`, `CARDStat` and THP types match (`os.h`'s
`check_layouts.hpp`). Include the NAND header before the port's own: the SDK defines the `NAND_*`
constants as macros.

`starlet/compat/*.h` let game code written for Metrowerks and Gekko compile on AArch64.

- Big-endian field types (`big_endian.h`). Game resource files stay big-endian in memory, as on
  the console, because the GX reads vertex arrays, textures and display lists straight from them.
  Structs that overlay file data declare multi-byte fields with `BE<T>`, which has T's size and
  alignment and converts on every read and write. Variants: a constant table of file-format words
  stored big-endian at compile time; a pointer inside a 32-bit slot of file data (game memory
  lies below 4 GiB, so the native 32-bit value holds it); a 32-bit address slot the game relocates
  in place (a big-endian file offset until the base is added, then a big-endian pointer; addresses
  with the sign bit set are pointers); and a big-endian word read at an unaligned address.
- Gekko intrinsics. `ppc_intrinsics.h` has the integer and cache intrinsics (`rlwimi`, `rlwinm`,
  byte-reversed loads; cache maintenance does nothing on coherent host memory; `dcbz` zeroes the
  32-byte block). `ppc_float.h` has the floating-point ones as macros, because glibc's `math.h`
  declares `__fabs` and friends as functions. `frsqrte` and `fres` return the exact value: games
  refine them with Newton steps, which leave an exact input unchanged. `fsel` selects b for
  a >= 0 (including -0), c for negative a or NaN. `ppc_paired.h` has paired-single operations as
  lane-wise single-precision code in the same order, with fused multiply-adds as on the hardware.
  Scalar single-precision instructions write both lanes, as Gekko does in paired-single mode; `fmr`
  writes ps0 only. Quantized loads and stores cover GQR0 (unscaled f32) only; other cases are
  converted at their call sites.
- Quantization registers (`gqr.h`). A GQR holds the load type in bits 16-18, the load scale in bits
  24-29, the store type in bits 0-2 and the store scale in bits 8-13. Types: 0 f32, 4 u8, 5 u16, 6
  s8, 7 s16. Scales are signed 6-bit powers of two. Loads multiply by 2^-scale, stores by 2^scale,
  clamp to the type's range and truncate. Quantized words in memory are big-endian.
- Write-gather pipe (`gx_fifo.h`): see [OS-and-GX-FIFO](OS-and-GX-FIFO.md).
