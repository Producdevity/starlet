# Host services: audio, controllers and quit

`starlet::host` holds the SDL2 services every port needs on a handheld: audio output, game
controllers and the frontend's quit request. It uses the firmware's SDL2 and its default routes
and never changes volume, mute, mixer, clock or controller settings. Production code calls SDL
2.0.9 only, the version of the target sysroot's headers. The public headers do not include SDL.

## Audio output

`AudioOutput` opens the default device at the port's rate, s16 stereo, asking for a 512-frame
buffer (firmware often grants more). SDL converts to whatever the device plays; the port's
samples reach SDL unchanged. Galaxy and Pikmin 2 play the console's 32028 and 48043 Hz
(32028.5 and 48043 Hz rounded), gc-recomp the nominal 32000 and 48000 Hz.

Frames travel through a lock-free single-producer, single-consumer ring (`audio_ring.hpp`) to
SDL's callback. Storage is a power of two, so a frame's slot survives the wrap of the 32-bit
counters on armhf. The device plays silence until the ring first holds the device buffer plus
four 5 ms slices; from then on a request the ring cannot fill gets silence for the rest and
counts in `underruns` and `underrun_frames`.

Two ways to feed it:

- `open_pull(rate, pull, context)` is the AI DMA model of Galaxy and Pikmin 2. A Starlet thread
  (`starlet-audio`, SDL's high priority) pulls 5 ms slices on a steady clock and keeps the device
  buffer plus four slices queued: below two slices it pulls extra, above eight it skips a tick,
  and a stall of more than four ticks restarts the clock. A device that takes 2048 frames or more
  at once then still sees the game's DMA completions evenly spaced, and the queue absorbs drift
  between the device's clock and the host's. Pulls are serial; none runs once `close()` returns.
  The port's model keeps its own waits and repeats (the DMA replaying a buffer the game was late
  to refill).
- `open_push(rate, buffer_ms)` takes frames from the port's thread through `write()`, as
  gc-recomp's AI DMA produces them. The ring holds `buffer_ms` (120 ms by default, at least the
  start level plus four slices); a write that does not fit is dropped whole and counts in
  `dropped_writes` and `dropped_frames`, which bounds latency when the game runs ahead.

Without a device (no SDL audio driver, no card, a refused format) `device_error()` says why,
`stats().device` is false, and a silent clock (`starlet-silence`) consumes the ring at the rate,
so the game's DMA keeps finishing blocks. With the firmware volume at 0 the device plays
normally; `frames`, `nonzero_samples` and `peak` show the game produced sound.

Every open logs SDL's audio driver and the device buffer it granted, and a pull output logs the
playing ALSA devices' `hw_params` and `status` once, about 3 s in. `STARLET_AUDIO_PROBE=<file.wav>`
records what the device callback receives (the samples the speaker path is given, at the output
rate, the first 600 s unless `STARLET_AUDIO_PROBE_SECONDS` says otherwise) and logs the callbacks and
their longest interval, the ring's lowest level and the underruns for every second that had a late
callback, an underrun or a drop, and every 30 s otherwise, plus the ALSA status every 5 s. For
development only.

`open_*`, `close` and the destructor allocate host memory; a port with its own `operator new`
(Galaxy's game arenas) calls them inside its host-allocation scope.

## Controllers

`Controllers(slots, log)` starts SDL's game-controller subsystem, which reads the firmware's
mappings from `SDL_GAMECONTROLLERCONFIG` (a PortMaster launch script exports
`$sdl_controllerconfig` there after `get_controls`). `update()`, on the thread that pumps SDL's
events, updates SDL's controller state, closes removed controllers, opens new ones into free
slots in SDL's device order, and samples each slot; `state(slot)` returns the latest sample from
any thread. A removed controller leaves its slot empty until another arrives. The log callback
gets one line per event:

- `Controller 1: <SDL mapping>` when a controller opens;
- `Controller 1: removed`;
- `Controller: none of N joysticks has an SDL mapping: <GUID> <name>, ...` while no controller is
  open, again when the joystick count changes. The GUID has bytes 2 and 3 cleared, as mappings
  carry it (newer SDL keeps a CRC there), so a tester's log line gives the
  `SDL_GAMECONTROLLERCONFIG` entry to add.

A `Gamepad` is SDL's view: the 15 SDL 2.0.9 buttons by place (`south`, `east`, `west`, `north`
are SDL's A, B, X, Y), raw stick axes with y down, triggers 0..32767.

### GameCube layout

`to_gamecube(pad, mapping)` produces a `GameCubePad` as PADRead reports it: `PAD_BUTTON_*` bits,
sticks in the console's signed range with up positive, analog triggers 0-255. `GameCubeMapping`
is the port's data:

- each GameCube button lists the `Input`s that press it;
- `trigger_threshold`: a trigger presses above it;
- `trigger_left`, `trigger_right`: the analog value from the trigger axis (value / 128, via
  `from_axis`) and/or 255 while the digital L or R is pressed (`full_when_pressed`);
- `main_stick`, `c_stick` (the left and right sticks): a `StickResponse`.

`StickResponse` holds the dead-zone shape (radial on the stick's length, or axial on each axis),
the dead zone (inclusive), whether values rescale from the dead zone's edge, the raw deflection
that reads as 1, an optional limit to the unit circle, and the console's value at 1.
`read_stick` returns the response as floats for a port that maps sticks itself; `stick_value`
scales and rounds half away from zero.

The `gamecube` shape models a real controller feeding the SDK's stick clamp. The SDL vector is
limited to the unit circle and scaled to `gate_radius` (the raw value at full deflection, about
87 for the control stick and 74 for the C-stick), each axis loses `gate_dead_zone` (15), and the
result is limited to an octagon: cardinal maximum `range`, diagonal corner (`gate_corner`,
`gate_corner`), with a point beyond an edge scaled toward the centre onto it. The output is whole
console units divided by `range`; `dead_zone`, `rescale`, `full` and `circle` do not apply.
Control stick: `gate_radius` 87, `range` 72, `gate_corner` 40. C-stick: 74, 59, 31.

The ports' layouts, reproduced in `tests/host/pad_tests.cpp` against transcriptions of their code:

| | Pikmin 2 | gc-recomp |
|--|--|--|
| Face buttons | printed labels: A east, B south, X north, Y west | GameCube places: A south, B west, X east, Y north |
| L, R | shoulders | L: left shoulder or trigger; R: right trigger |
| Z | either trigger | right shoulder |
| Trigger threshold | 16000 | 24000 |
| Analog L, R | 255 while pressed | trigger axis; L also 255 while pressed |
| Sticks | radial, dead zone 8000, rescaled, 32767 full, main 72, C 59 | axial, dead zone 3932, not rescaled, 32768 full, 100 |

gc-recomp rounds `128 + x * 100`, so where `x * 100` is exactly -n.5 (a deflection of 4096,
12288, 20480 or 28672 to the left or down) its byte is one higher than 128 plus Starlet's signed
value; every other value matches.

Galaxy maps the handheld to a Wii Remote and Nunchuk itself from the `Gamepad`: face buttons by
printed label, both shoulders and both triggers (16000) as separate controls, Start, Guide and
Back, and `read_stick` with an axial dead zone of 8000, rescaled, 32768 full, no circle limit,
range 71.

## Quit

The frontend ends a port with SIGTERM. gptokeyb's exit chord sends it, waits 3 s and sends
SIGKILL (gptokeyb `doKillMode`). The contract: **a port flushes its saves and exits within 1 s of
SIGTERM.**

- `install_quit_signals()` installs SIGTERM and SIGINT handlers that only set an atomic flag and
  write a byte to a pipe (both async-signal-safe), with `SA_RESTART`. Once per process. Install
  it before or after `SDL_Init`: SDL installs its own handler only over the default one and
  removes only its own.
- `poll_events(handler)` replaces the port's `SDL_PollEvent` loop, on the thread that owns SDL's
  events: `SDL_QUIT` (a window close, a desktop session) requests the quit too, and every event
  goes on to the handler.
- `quit_requested()` is the check for the main loop; `quit_signal()` tells a log which signal
  asked (0 when code did). `wait_for_quit(timeout)` and `wait_for_quit()` block on the pipe. The
  byte stays in the pipe, so any number of threads wake at once and every later poll returns.
- `on_quit(flush)` registers a save flush. `exit_now(status)` runs the flushes once, in
  registration order, on the calling thread, catching exceptions, then flushes stdio and
  `_Exit`s. A second thread calling it waits for the first; a flush that calls it ends the
  process at once.
- `exit_after_quit(grace)` starts a `starlet-quit` thread that calls `exit_now(0)` `grace` after
  the request: the exit for a main loop that is stalled in a load, a guest wait or a shader
  build. A flush may then run on that thread while the game thread is mid-frame, so a flush locks
  what it writes and waits for no thread that can itself call `exit_now`.

A port checks `quit_requested()` once per frame and wherever it waits, calls `exit_now(0)`, and
arms `exit_after_quit` with a grace that leaves room for its flush inside the second (500 ms suits
memory-card and NAND flushes of a few hundred KiB).

## Build switches

`src/host/CMakeLists.txt` finds `SDL.h` (under `SDL2/`) and `libSDL2` the way the ports do: the
target sysroot's in a cross build, Homebrew's sdl2-compat on a Mac (`STARLET_SDL2_INCLUDE_DIR`,
`STARLET_SDL2_LIBRARY`). `STARLET_HOST` defaults to ON when both are found; a gx- or disc-only
consumer without SDL2 is unaffected.

`tests/host` runs with `SDL_AUDIODRIVER=dummy` and `SDL_VIDEODRIVER=dummy`; on a Mac CTest points
`DYLD_FALLBACK_LIBRARY_PATH` at the SDL2 library's directory, where sdl2-compat finds SDL3. The
controller tests use SDL virtual joysticks (SDL 2.0.14 runtimes; they skip on older ones). Each
quit test runs in its own process, since the quit request happens once per process.
