# AX

`starlet::ax` implements the Dolphin SDK's AX voice library for SDK-level ports:
the CPU side the SDK compiles (voice stacks, parameter-block setters, the
per-frame sync) and the DSP microcode's 5 ms frame. It plays through
`starlet::ai`, as the console's AXOut does through the AI DMA. First user: the
Resident Evil 4 port.

## What the port compiles

The port keeps the CPU-side libraries that sit on AX and replaces the AX units
with this module.

| Keep compiling | Stop compiling (Starlet defines them) |
| --- | --- |
| `syn*.c`, `seq.c`, `axart*.c`, `mix.c`, `axfx.c`, `reverb_*.c`, `chorus.c`, `delay.c`, `ax_rna.c` | `AX.c`, `AXAlloc.c`, `AXAux.c`, `AXCL.c`, `AXOut.c`, `AXProf.c`, `AXSPB.c`, `AXVPB.c`, `DSPCode.c`, `ai.c` (`starlet::ai`) |
| `AXComp.c`: the compressor's gain table `__AXCompressorTable`, which Starlet links against | |

SYN, SEQ, AXART, MIX and AXFX are CPU code: they compute envelopes, pitch,
pans and effects and write parameter blocks, so they run unchanged. The AX units
are the DSP's half (`DSPCode.c` is the microcode image, `AXOut.c` drives the DSP
mailbox and the AI DMA) and the CPU code that hands parameter blocks to it; their
state is shared with the DSP model, so Starlet owns all of them. `AXComp.c` is
data the microcode reads; the port links it instead of Starlet copying it.

C symbols defined here: `AXInit`, `AXInitEx`, `AXQuit`, `AXAcquireVoice`,
`AXFreeVoice`, `AXSetVoicePriority`, `AXRegisterAuxACallback`,
`AXRegisterAuxBCallback`, `AXSetMode`, `AXGetMode`, `AXSetCompressor`,
`AXSetStepMode`, `AXRegisterCallback`, `__AXLocalProfile`, `AXInitProfile`,
`AXGetProfile`, `AXSetVoiceSrcType`, `AXSetVoiceState`, `AXSetVoiceType`,
`AXSetVoiceMix`, `AXSetVoiceItdOn`, `AXSetVoiceItdTarget`,
`AXSetVoiceUpdateIncrement`, `AXSetVoiceUpdateWrite`, `AXSetVoiceDpop`,
`AXSetVoiceVe`, `AXSetVoiceVeDelta`, `AXSetVoiceFir`, `AXSetVoiceAddr`,
`AXSetVoiceLoop`, `AXSetVoiceLoopAddr`, `AXSetVoiceEndAddr`,
`AXSetVoiceCurrentAddr`, `AXSetVoiceAdpcm`, `AXSetVoiceSrc`,
`AXSetVoiceSrcRatio`, `AXSetVoiceAdpcmLoop`, `AXSetMaxDspCycles`,
`AXGetMaxDspCycles`, `AXGetDspCycles`, `AXSetVoiceLpf`, `AXSetVoiceLpfCoefs`,
`AXGetLpfCoefs`. The SDK's `__AX*` internals and `axDspSlave` are not defined;
nothing outside the AX units uses them.

`include/starlet/sdk/ax.h` declares the types with the SDK's field order and
native pointers. A port asserts its decompilation's `AXVPB`, `AXPB` and
`AX_AUX_DATA` against them (include Starlet's header first, with
`STARLET_SDK_TYPES_ONLY`, as for `os.h`).

## Frames

The game calls `AIInit`, then `AXInit` or `AXInitEx(mode)`. AXInit registers
an AI DMA callback, programs a silent buffer and starts the DMA. Each AI
interrupt, on the guest CPU, is the SDK's `__AXOutAiCallback`:

1. Sync: copy what the game changed in each voice's block into the DSP's copy,
   by sync flag, and copy the DSP's state, address and envelope volume back.
   Voices are visited by priority, 31 first; one that pushes the SDK's cycle
   estimate past `AXGetMaxDspCycles` (405000 by default) stops and its drop
   callback runs in step 3. `AXGetDspCycles` returns that estimate.
2. The DSP frame (`dsp.cpp`) runs with the command list built in the previous
   interrupt and mixes into one of three output buffers.
3. Drop callbacks, aux callbacks, the game's frame callback, then the next
   command list (mode, aux buses with a callback, compressor).
4. Mode 1 (triple buffering, RE4's) programs the buffer mixed one interrupt
   earlier; mode 0 programs the buffer just mixed.

The DSP finishes within the interrupt, so the SDK's late-DSP path
(`lessDspCycles`) is always 0. `starlet::ai` paces the interrupts at the
device's rate and replays a block when the guest is late; `starlet::ai::stats`
counts those. AXInit's silent first block plays twice, as on the console.

## The DSP frame

After Dolphin's AX HLE (GPL-2.0-or-later, dolphin-emu/dolphin
`Source/Core/Core/HW/DSPHLE/UCodes/AX.cpp`, `AXVoice.h`, `AXStructs.h`) and its
accelerator (`Source/Core/Core/DSP/DSPAccelerator.cpp`):

- Setup: each of the nine buses (main, aux A, aux B; L, R, S) starts at the
  depop value of dropped voices and steps toward zero by at most 20 a sample
  (`AXSPB.c`).
- Stereo mode adds the previous frame's S bus to L and R; surround mode adds it
  to R and subtracts it from L. Dolphin assigns instead of adding, which
  discards the main buses' depop ramps; Starlet keeps them.
- Voices, in block order, each millisecond (32 samples): apply the block's
  per-millisecond updates, then for a running voice read samples through the
  accelerator and converter, apply the envelope (`ve`, signed volume, per-sample
  delta), the one-pole low-pass (`lpf`), and add to each bus enabled in
  `mixerCtrl` with its volume and optional per-sample ramp. Each bus
  contribution saturates to 16 bits. The last sample per bus is kept as the
  voice's depop value.
- Accelerator: ADPCM (4-bit, 8-byte frames with a predictor/scale header,
  nibble addresses), PCM16 and PCM8 (sample addresses, big-endian in ARAM, gain
  0x800 and 0x100 set by `AXSetVoiceAddr`). Reading the sample at the end
  address loops to the loop address with the loop predictor/scale and, unless
  the voice is a stream, the loop history; without the loop flag the voice
  stops.
- Converter: 16.16 ratio up to 4.0. Type 0 is 4-tap polyphase with 128 phases,
  type 1 linear, 2 none. Both interpolating types use a four-sample history,
  oldest first, as Dolphin. The coefficient tables (`coefSelect` 0-2 for 8, 12
  and 16 kHz) are generated at start with the windowed-sinc formula of
  Dolphin's free DSP ROM (`docs/DSP/free_dsp_rom/generate_coefs.py`); the
  console's ROM tables differ slightly.
- Aux: with a callback registered, the aux bus goes to the CPU's buffer and the
  buffer the callback processed a frame earlier returns into main L/R/S, two
  frames after the send (`AXAux.c`'s three buffers). Without a callback the bus
  is dropped. DPL2 mode (2) sends aux A L/R/S and aux B S to the aux A callback
  (`AX_AUX_DATA_DPL2`), returns them to main L/R and aux B L/R, then loops aux
  B L/R (the rear channels) back into main L/R one frame later.
- Compressor (on by default): when main L or R exceeds 32768, the frame is
  scaled by an attack frame of `__AXCompressorTable`; then ten release frames
  follow.
- Output: L and R saturate to 16 bits, right channel first in the buffer as
  the AI DMA reads it; S is kept for the next frame.

## Not modelled

- Initial time delay (ITD). The SDK fields are synced; the DSP mixes without the
  delay, as Dolphin. A voice with a nonzero shift logs one line. RE4 enables ITD
  only through AXART sounds with zero targets.
- FIR (`AXSetVoiceFir`): stored and synced; no AX microcode uses it.
- `COPYALL` sync copies the game's update words to the DSP. The SDK copies them
  the other way, which loses them; nothing in RE4 uses it.

## Tests

`tests/ax` runs AX on the real AI model with a hand-driven guest CPU and output:
ADPCM decoding against hand-computed samples, PCM looping and stopping, ADPCM
loop context for voices and streams, SRC sample counts at 1.0 and 0.5,
per-sample volume ramps across frames, aux A routing and latency, DPL2 rear
routing, the compressor, priority drops, budget drops and the frame-callback
cadence including a late guest.
