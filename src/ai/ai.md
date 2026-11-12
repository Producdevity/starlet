# AI

`starlet::ai` implements the audio interface's DMA for SDK-level ports, from
the Pikmin 2 port's AI model. It knows nothing of AX or JAudio: the game, or
`starlet::ax`, programs blocks and handles the interrupt.

## DMA

`AIInitDMA` (guest address) and `starlet::ai::init_dma` (host pointer) copy the
block they program. Blocks are native s16 stereo, right channel first, 32-byte
aligned, a multiple of 32 bytes. `AIStartDMA` plays the programmed block. When
the playing block finishes, the DMA latches the programmed block and raises the
AI interrupt on the guest CPU, which calls the DMA callback; the game then
programs the block after it. If nothing new is programmed within 12 ms of a
block's end, the DMA replays the last programmed block, as the hardware does
when software is late. `stats()` counts completions, waits, repeats and the
time spent waiting.

## Output

`AIInit` opens `starlet::host::AudioOutput` in pull mode (`src/host/host.md`):
selector 0 plays at 32028 Hz and 1 at 48043 Hz (the console's 32028.5 and
48043 Hz). Its thread pulls 5 ms slices on a steady clock; the DMA swaps the
channels for the device. Without a device a silent clock pulls at the rate and
`device_error()` says why. Volume, mute and mixer settings stay as the firmware
has them. `AISetDSPSampleRate` reopens the output at the new rate; it aborts if
the DMA runs.

`set_capture(path)`, before AIInit, records what the DMA plays to a 16-bit
stereo WAV.

## Registers without hardware

`AIReset` clears the init flag, as the SDK; the DMA and the output keep
running. The stream (disc audio, DTK) volume and play-state registers hold
their values; starting a stream logs one line and the stream stays silent.
`AIRegisterStreamCallback`, the stream sample counter, trigger and stream rate
are not implemented: no current port calls them.

## Tests

`tests/ai` drives the DMA by hand: channel order, latching and one interrupt
per block, replay of an unprogrammed block, a stopped DMA, guest addresses and
queued interrupts, the 48 kHz rate, the stream registers and AIReset.
