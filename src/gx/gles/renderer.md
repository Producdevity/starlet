# GLES integration

Build with `STARLET_GLES=ON` and the target's GLES3.1 headers/library. The frontend
owns SDL and keeps the same context current on the constructing thread through
renderer destruction. The renderer owns a 640-wide RGBA8/D24 EFB of the frame height
in `GlesLimits::efb` (decoder.md) and presents it to the drawable. It accepts the explicit `plan_gles_draw` domain and the native
copy/presentation domains documented in `display_copy.md` and `texture_copy.md`.

The direct draw(state, command, memory) entry owns an owner-thread decoding cache.
It compares current source and palette bytes before reusing immutable mip levels.
A memory reader may expose a bounds-checked canonical span for that synchronous
comparison, avoiding a temporary image allocation/copy on a cache hit. Neither
snapshots nor the cache retain borrowed spans; misses retain owned source bytes.
Typed native arrays still use canonical conversion. Source addresses alone never
validate cached pixels. A per-unit last-hit hint changes lookup order only; every
candidate must match the full content key. The packet entry accepts owned snapshots. The cache uses the
renderer texture byte/entry limits as a separate CPU budget (32 MiB/128 by
default), including retained metadata, source bytes and decoded levels. Sampler state and palette generations
are captured afresh. Its byte budget includes retained entry capacity and vector
payloads, excluding allocator overhead; packets may separately retain evicted data.

The renderer retains textures by owned RGBA content, dimensions and admitted
sampler state, so changing map slots does not force another upload. LRU eviction
protects every texture used by the current draw. Texture bytes and object counts
are bounded separately; program retention is also capped. Every draw validates
its current vertices, uniforms and canonical shader key. Immutable CPU transform
controls are prepared once per draw; per-vertex arithmetic checks remain.
Identical lighting functions reuse their per-vertex factors across RGB/alpha
channels, retaining independent light masks and color inputs. GLSL source is
generated only on a compiled-program cache miss; a warm cache never bypasses
input validation. With `GlesDirectLimits::program_cache` set, the direct path
also keeps linked program binaries on disk (`GlesProgramCache`), one file per
program keyed by the driver's vendor, renderer and version strings and the exact
GLSL sources, which the file stores and compares byte for byte. A checksum
rejects torn writes before the driver sees them; a binary the driver refuses is
deleted and the program is compiled again. Entries are written to a temporary
name and renamed; above 4096 entries the least recently written are removed down
to 3072. With fast shading, the words of each linked program are appended to
`programs.list` in that directory (`ProgramList`); `direct_program_state`
rebuilds a State whose words match, so such lists stay valid across
shader-generator changes and devices. `GlesProgramBuilder` compiles listed
programs the cache lacks on two threads, each with its own surfaceless or
pbuffer EGL context in the renderer's share group, while the application shows
progress before starting the game: on Mali, compiles beside the running game
stall its GL thread. After `build_programs_in_background()`, one low-priority
thread builds the rest and the programs the GL thread finds missing; draws using
such a program are skipped until its binary can be loaded, instead of stalling
on the compile. Projective coordinates first use the original-vertex proof. If
that is inconclusive, a bounded homogeneous clipper checks the surviving
polygons; offscreen singularities cannot reject a valid visible projection.
Visible zero-Q crossings and overflow fail with an error. Neither cache
assumes that an address is an immutable resource generation. These limits
exclude driver/compiler internals, in-flight allocations and CPU packet storage.
A previous-frame fence is retired at presentation; the frontend must present
regularly and bound submitted work per frame. On the direct path a retrace whose
XFB, extent and black state match the previous presentation, with no display
copy into that XFB since, presents nothing and the caller does not swap: the
earlier image stays on screen, so logic-only frames cost no presentation pass.

The synchronous `draw_batch` entry accepts up to 4096 owned packets. Every
packet is validated before driver mutation. Batches of at least 64 packets prepare
CPU plans on the owner plus at most two persistent workers, bounded by reported
CPU concurrency. Workers read owned immutable snapshots and issue no GLES
commands. All results and exceptions are joined before driver mutation; invalid
packets reject the entire batch, and later valid batches can reuse the workers.
Small batches retain serial preparation. Only adjacent packets with identical
shader, raster, uniform and sampled image state share a submission. Geometry
keeps its original values and triangle order; indices are offset into at most
65,535 vertices and 196,605 indices per submission. State changes or these bounds
start another submission. No work remains queued after return, so callers keep
copy, clear, completion and presentation boundaries explicit. `draws` counts
nonempty input packets and `submissions` counts indexed driver draws.
Temporary merged geometry is at most 9,043,830 bytes plus bounded planning data;
caller-owned snapshots and driver allocations have separate lifetimes/budgets.
Mali-G31 comparisons cover RGB8 blending, RGBA6 coherent fetch, depth rejection,
source mutation and state restoration (14,008,320 color bytes and 108 depth
samples). A recorded file-menu segment also produced identical replayed color
bytes: 188 compatible packets became one submission. This replay starts from a
controlled clear and excludes capture, copies and simulation; it is not a full
frame or gameplay benchmark.

Parallel preparation also passed the same Mali color/depth cases with 160-packet
batches. Eight alternating trials of the same 188-packet menu segment produced
identical color bytes and one driver submission in both modes. Median warm
preparation/submission/completion time was 22.17 ms serial and 16.25 ms parallel
(about 27% less). Capture, simulation and readback are excluded.

Scalar uniforms are owned by each cached program. Only active locations with
changed byte values are uploaded; program changes retain their own values and
eviction discards the cache with the program. The owned transform buffer is
reuploaded only when its complete bytes change; it is rebound for every draw.
Geometry remains per draw. No shader arithmetic or ordering changes. Host/SP
dispatch checks cover changed/restored values and eviction. A muted Mali-G31
comparison found identical pixels for 18 cached/forced-upload cases (21,012,480
RGBA bytes), including visible color/transform changes. A repeated 256-draw
fixture reduced buffer uploads from 768 to 512.

Pixel transfers reset row/skip/PBO state; consumed units unbind sampler objects.
Any failure after GL mutation permanently disables further renderer operations
except stats/destruction. The driver state cannot reliably be rolled back.
Validation failures before mutation remain recoverable.

Boundary tests use a fake dispatch to exercise failure/ownership/state contracts.
They build when GLES headers are available, without linking a real GL library.
The cached command path additionally checks correct original/changed/restored
upload bytes, sampler variants, source-read failures, budgets and failure/thread
gates on host sanitizers and SP using fake GL dispatch. An alternating SP CPU
benchmark of 120 original strap-layout draws measured 27.1-27.5 ms/frame without
decoding reuse and 2.9-3.2 ms/frame with it, retaining 1,347,584 bytes. This excludes
GPU work, swap, simulation and full-game resource initialization; it is not a
measured gameplay FPS improvement.

H700 GPU validation separately compared nine textured gradient cases,
including same-address mutation, prior snapshots, filtering, scaling, scissor
and alpha rejection.

A full constant untextured replacement may switch EFB formats in either direction.
The RGB8 direction additionally requires blending disabled and proven opaque TEV
output; that draw restores the backing alpha plane to 255. Partial or blended
draws cannot reinterpret an existing format.

Host bloom (`begin_bloom`/`end_bloom`, enabled by the frontend for fast direct
shading) swaps the EFB colour attachment for a spare cleared to opaque black and
keeps depth, so the effect's mask draws need no copy or fill. End reads the mask
directly: a 160x114 pass takes each 4x4 EFB block's box average (four bilinear
taps, equal to the half-scale copy drawn at half size) and keeps it where its I8
luminance exceeds the threshold; a 12-tap pass blurs that at 160x114 and a
36-tap pass blurs the result at 80x57, each tap weighted by the effect's 8-bit
TEV colour and clamped once, as the EFB accumulation clamps. The scene colour is
then reattached and one additive full-screen pass adds both levels scaled by the
intensity, without writing alpha. Copies keep 8-bit channels and the 80x57 level
samples the 160x114 one rather than its box-filtered half; on SP the
consecutive-frame comparison against the GX passes differed by a mean of 0.1-0.4
in static regions.

A resident texture copy whose clear covers the whole EFB and rewrites an attachment the
copy does not read (depth for colour copies; colour, alpha included, for depth copies)
invalidates that attachment first, so the pass the copy ends does not write it back.

Offscreen passes (`begin_offscreen`/`end_offscreen`) swap both EFB attachments for
an EFB-sized colour and depth pair cleared to transparent black and the far plane.
Draws, clears and texture copies in between use the pair; ending reattaches the EFB
unchanged. This is the PC equivalent of the console's save-and-restore of an EFB
corner: Twilight Princess's PC build renders its bloom chain this way through
Aurora's `GXCreateFrameBuffer`. Offscreen passes and host bloom do not nest.

## Host GL thread

A port whose own renderer owns the GL context hands Starlet's GL work to that
thread. Construct the renderer there, then call `start_submission_on(post)`: `post`
receives each task, and the host runs it on its GL thread in order. Call
`adopt_thread()` on the thread that records draws. `end_frame()` submits the frame's
pending work. On the GL thread, `present_into(framebuffer, width, height, x, y, w, h)`
draws the frame's top-left `width x height` pixels into a rectangle of the host's
framebuffer without swapping, and the host presents. The renderer leaves GL state
changed after its tasks; the host invalidates its own state cache after them.
