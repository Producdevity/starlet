# GLES renderer

`GlesRenderer` (`starlet/gx/gles/renderer.hpp`) draws GX state into a GLES 3.1 EFB and presents it. The
direct path (`GlesDirect`, `starlet/gx/gles/direct.hpp`) records draws on the owner thread and submits
them to the GL thread in segments. The decoded GX state, draw packets and copy plans come from the GX
model: see [GX model](GX-model.md).

Build with `STARLET_GLES=ON` and the target's GLES 3.1 headers and library.

## Context and thread ownership

- The frontend owns SDL, the window and the swap. It keeps one context current on the constructing
  thread through renderer destruction.
- The renderer owns a 640-wide RGBA8/D24 EFB of the frame height in `GlesLimits::efb` and presents it to the
  drawable. A draw, copy or clear planned for another layout fails.
- Owner thread: records draws, issues copies, lands write-backs. `adopt_thread()` moves ownership to the
  calling thread, which must have the context current.
- Submission thread (`start_submission(release, bind, unbind)`): GL work runs there in call order while the
  owner keeps recording. It also runs EFB clears, copies, display copies and presentation.
  `stop_submission()` submits pending draws and joins; the owner rebinds the context afterwards.
  `Target::execute` decides where segments run: inline, or on this thread.
- Without a submission thread, GL work runs inline on the calling thread.

### Host GL thread

A port whose own renderer owns the GL context hands Starlet's GL work to that thread:

1. Construct the renderer on the host GL thread, then call `start_submission_on(post)`. `post` receives
   each task; the host runs it on its GL thread in order.
2. Call `adopt_thread()` on the thread that records draws.
3. `end_frame()` submits the frame's pending work and retires the previous frame, without presenting.
4. On the GL thread, `present_into(framebuffer, width, height, x, y, w, h)` draws the frame's top-left
   `width x height` pixels (the game's display-copy source) into a rectangle of the host's framebuffer, lower-left
   origin, without swapping. The host presents.

The renderer leaves GL state changed after its tasks; the host invalidates its own state cache after them.
`stop_submission()` waits for posted tasks.

### Frames in flight

A previous-frame fence is retired at presentation. The frontend must present regularly and bound the work it
submits per frame. `set_frames_in_flight(2)` retires the fence of the frame two presentations back, so the GL
thread queues a second frame before the GPU finishes the first (one frame more latency). Peeks, readbacks and
materialized copies still wait for all work before them.

On the direct path, a retrace whose XFB, extent and black state match the previous presentation, with no display
copy into that XFB since, presents nothing and the caller does not swap: the earlier image stays, so logic-only
frames cost no presentation pass.

## Failure latching

- Validation failures before any GL mutation are recoverable and leave the renderer usable.
- A failure after GL mutation latches: driver state cannot be rolled back. Only `stats()`, `failed()` and
  destruction remain available.
- A GL task that fails on the submission thread latches too; the recording thread sees it at its next call.
- Pixel transfers reset row, skip and PBO state; consumed units unbind sampler objects.

## EFB attachments

- The EFB is RGBA8 colour plus D24 depth. RGB8 and RGBA6 frames are stored as RGBA8.
- A full constant untextured replacement may switch EFB formats in either direction. The RGB8 direction
  needs blending off and proven opaque TEV output; that draw restores the backing alpha plane to 255. Partial
  or blended draws cannot reinterpret an existing format.
- `DirectShading::fast` stores RGBA6 as RGBA8, blended in all four channels with GX factors (destination alpha
  included), and never reinterprets on pixel-format changes, whether by draws, copies or clears. An SDK depth
  copy selects Z24 inside RGBA6 frames. It targets visual fidelity, not GX bit equality.
- Constant alpha replaces only the stored alpha; colour still blends with the TEV alpha (Freezeflame ice).
- A frame shorter than the EFB's 528 rows keeps the rows below it in a strip, a target of its own, so passes over
  the frame never load or store them. Draws whose viewport reaches past the frame's last row render into both
  targets. Copies across that row read both (two blits, or one copy program with a second sampler); their clears
  clear both. Fast copies that are neither are refused. A frame of all 528 rows has no strip. Peeks into the
  strip's rows read it live; depth peeks with no EFB write between them share one readback of its rows.
- Offscreen passes (`begin_offscreen`/`end_offscreen`) swap both attachments for an EFB-sized colour and depth pair
  cleared to transparent black and the far plane. Draws, clears and copies in between use the pair; ending
  reattaches the EFB unchanged. This is the PC equivalent of the console's save and restore of an EFB corner:
  Twilight Princess's PC build renders its bloom chain this way through Aurora's `GXCreateFrameBuffer`.
  Offscreen passes and host bloom do not nest.
- Host bloom (`begin_bloom`/`end_bloom`, for fast direct shading) swaps the colour attachment for a spare cleared
  to opaque black and keeps depth, so the effect's mask draws need no copy or fill. End reads the mask directly:
  - a 160x114 pass takes each 4x4 EFB block's box average (four bilinear taps, equal to the half-scale copy drawn
    at half size) and keeps it where its I8 luminance exceeds the threshold;
  - a 12-tap pass blurs that at 160x114 and a 36-tap pass blurs the result at 80x57, each tap weighted by the
    effect's 8-bit TEV colour and clamped once, as the EFB accumulation clamps;
  - the scene colour is reattached and one additive full-screen pass adds both levels scaled by the intensity,
    without writing alpha.

  Copies keep 8-bit channels and the 80x57 level samples the 160x114 one, not its box-filtered half. On SP the
  frame comparison against the GX passes differed by a mean of 0.1-0.4 in static regions.
- Copy and clear invalidation: a resident texture copy whose clear covers the whole EFB and rewrites an
  attachment the copy does not read (depth for colour copies; colour, alpha included, for depth copies)
  invalidates that attachment first, so the pass the copy ends does not write it back (`GlesStats::invalidations`).
  Copy programs invalidate their targets before drawing, so the pass does not load the targets' old texels.
- Clears: a constant clear without a format change waits for the next GL task and runs at its start, so it opens
  the pass that uses its attachments. Run at once, a clear followed by work on other attachments is a pass of its
  own that Mali stores and the next pass loads back (Twilight Princess's title clears the EFB and then renders
  an offscreen pass). A clear still waiting when an offscreen pass ends is dropped: nothing reads the pair
  afterwards and the next pass clears it whole. A clear of the whole EFB runs without a scissor test, since Mali's
  r20p0 driver clears under one with a draw that first loads the rest of the pass.

## Display and texture copies

- Display copies render into a per-XFB texture that VI presentation samples, so XFB memory is not written. Fast
  shading keeps XFB copies in RGB and skips the YUYV encode and VI decode. Because no CPU-visible GX result is
  pending, `wait_until_idle` only submits records; presentation still waits for the previous frame.
- `copy_texture_resident` keeps the copy on the GPU (below). Without the direct path it is a plain copy.
- `copy_texture_known` takes a copy whose encoded bytes the caller computed: earlier write-backs into its memory
  land first, memory takes the contents at once, nothing is read back. A copy that leaves the EFB as it is renders
  nothing: resident copies of the range are dropped as by a host write. One that clears the EFB is kept resident
  as well.

### GPU residency

An EFB texture copy stays on the GPU: a shader writes the texels the copy's sample format decodes
(`decode_texture_copy`). In fast shading, copies other than luminance and RGB5A3 are framebuffer blits whose
target swizzle selects the decoded channels, without 4/5/6-bit quantization. Memory at the destination is stale
until the copy is materialized.

Lookups at an address a resident copy covers:

| Lookup | Result |
| --- | --- |
| Same address, format, size, one level, natural tile stride | Samples the copy directly. |
| Smaller, natural stride equals the copy's | Reads the copy's top-left texels from a texture of its own size, filled by a framebuffer blit on the GPU and kept until the copy is replaced; it wraps and clamps at the lookup's edges. |
| Taller I8 lookup of the copy's width (Super Mario Sunshine: 512x512 over a 512x448 copy) | The copy's rows on the GPU, the rows below from memory, uploaded first and decoded as the materialize path would (`decode_texture_copy_tail`). The tail is hashed on every lookup; a change rebuilds the texture. |
| Any other overlap | Materializes first: a readback with a fence wait, so the GPU thread waits for all queued GL work, then `store_texture_copy` reproduces the CPU encoder's bytes. |

Crops use a framebuffer blit, not `glCopyTexSubImage2D`: on Mali's vendor driver (r20p0) that waits for the GPU to
finish the copy's pass (Sirena Beach: 21 ms per frame on the GL thread). `STARLET_GX_SYNC_STATS=1` logs each
lookup that still materializes, once per case, with the reason.

`release_resident` drops the copies in a range the port no longer lets the GX write (Galaxy unregisters its
writable ranges this way).

### Merged passes

Resident texture copies wait for the next renderer call. Copies with no other call between them read the same
EFB. Those whose pass differs only in its target (same rectangle, scale and EFB format, and the same blit or
the same copy program and uniforms) render in one pass: one blit into up to four targets' draw buffers, or one draw of a
program that writes its value to up to four outputs. Each pass costs on Mali-G31 r20 whether it is large or
small. A copy that clears renders at once, after the copies queued before it, so the copy after it reads the
cleared EFB. `GlesStats::merged_copies` counts copies that shared a pass.

### Stamps

Where nobody reports CPU writes (a recompiled game, or a CPU copy such as Resident Evil 4's `MemorySwap`),
`stamp_resident_copies` writes a per-copy stamp into the first eight bytes of up to 64 destination blocks spread
over the copy, its first and last included. The copy is sampled or materialized only while every stamp is intact;
a game reusing the memory overwrites it wholesale and breaks one, and the memory then keeps the game's bytes.
Stamping every block cost a cache miss per block, several milliseconds per frame on the H700.

### Overlapping copies

A copy that overlaps part of an older one takes over only those bytes. The older copy keeps its texture and records
the ranges newer copies own (`covered`), and is retired once they cover all of it. A lookup reaching its remaining
bytes materializes it, writing only those bytes, together with the current copies the lookup overlaps. This is
exact: the texture is never rendered into again, and every lookup checks older copies before reading memory.
Remaining bytes are disjoint from every other copy's, so materialization order does not matter. Releases and
stamps apply as to any resident copy; stamps in covered ranges are the newer copies' and are not checked. Beyond
eight older copies the oldest is materialized.

Resident Evil 4 writes its 256x196 RGBA8 refraction copy and then a 256x224, 128x112 and 64x56 I8 downsample chain at
the same address, each covering the start of the previous one; this removes three readbacks per frame.

### Host transfers

Host transfers on the game's behalf call `starlet::os::before_dma_read` or `before_dma_write` first: ARAM and disc
DMA, card reads and writes, THP planes and audio DMA. A port whose own DMA bypasses `starlet::os` calls the
renderer's `before_dma_read` and `before_dma_write` itself.

- A read materializes the copies it reaches, waiting for the GL thread's readback, so the transfer gets the copy's
  bytes.
- A write drops the copies it covers without materializing them. A copy it covers in part records the written
  range as covered and is kept like a copy a newer one partly replaced, so its other bytes stay exact.
- Cache stores and flushes only change page generations, since games flush memory holding valid copies before
  swapping it out.
- `gx_fifo` passes a transfer whose range holds a byte a resident copy owns (`resident_in`: each copy's
  destination and the uncovered ranges of partly replaced copies, republished by the GPU thread whenever its copies
  change) to the GPU thread after the commands queued before it, while the transfer waits. Other transfers never
  wait for the GPU thread or take a lock while at most 32 ranges are resident: they read the ranges from atomics
  under a sequence count (about 40 ns on the arm64 test host).
- Copies by the game's CPU are not transfers. Resident Evil 4 swaps its sub screen window with ARAM by `memcpy`
  through 64 KiB DMA buffers (`MemorySwap`), so only stamps show that the room's bytes replaced the sub screen's
  copies there.

## Write-backs

`GlesRenderer::write_back(token, bytes, on)` names ranges whose copies the game's CPU also reads, as on the
console where a copy writes main memory (Super Mario Sunshine's goop maps: Mario slips, sinks and burns by
them). Such a copy stays resident and is not stamped.

1. The GL thread reads the copy's target into a pixel-pack buffer behind a fence, and maps the buffer once the fence
   signals (after each task, and every millisecond while idle).
2. A copier thread copies the texels out, since the Flip's Mali maps such a buffer at about 85 MB/s (3 ms for
   Sirena's 256 KiB map a frame, which on the owner held up its decoding and so the game's draw-done waits).
3. `land_write_backs(through, wait)`, on the owner thread, stores the bytes as materializing would, outside newer
   copies' ranges, in issue order. It is true once every write-back up to `through` is written or dropped;
   `wait` reads back and blocks until then. `write_backs_issued()` counts copies issued.
4. A copy into memory whose earlier copy has not landed lands that one first, waiting for its readback if needed;
   made a frame earlier, it is usually read back by then.

A copy of one byte per texel (I8 sample format, whole 8x4 tiles, natural stride) is packed on the GPU into its own
tiled bytes, four to an RGBA8 pixel, so a quarter of the bytes are read back and landing is a copy: Sirena's
512x512 map is 256 KiB a frame instead of 1 MiB and an encode. Such copies issued together are packed and read back
together, their rows stacked in one target, once 2 ms pass on the owner thread without another (at the latest at
the next display copy, or when a landing needs one). Each readback costs the Mali GL thread about 1 ms whatever
its size, and Bianco Hills copies five goop maps a frame back to back. Other copies are read back right after
their pass.

Until landing, and after, the copy is intact while memory holds the bytes it held when the copy was made, or the
landed bytes. A game that rewrote the memory first keeps its bytes, and the copy is dropped at its next lookup. A
copy released or materialized before its readback arrived lands nothing. A landed copy materializes without a
readback and is still sampled from the GPU. A written-back copy compares all its memory once a frame (Sunshine
samples its goop map in some 30 draws a frame); a CPU write later in the frame is seen at the next frame's first
lookup.

`GlesStats::write_backs`, `write_backs_dropped` and `write_back_waits` count landed, dropped and waiting copies.

## Snapshots and EFB peeks

- Peeks outside draw-sync callbacks read the live EFB and wait for queued GL work.
- With the direct path, draw-sync callbacks are deferred against snapshots of the EFB's packed D24 depth and
  colour (`snapshot_efb`), read back asynchronously on the GL thread, so draw-sync peeks read CPU memory. Without
  the direct path, `snapshot_efb` declines.
- Snapshot resolution: `GlesDirectLimits::snapshot_scale` (1, 2 or 4 EFB pixels per sample). 0 keeps quarter
  resolution under fast shading and full resolution otherwise. Callbacks that test single pixels need 1.
  `snapshot_efb_exact` snapshots are full resolution, so their peeks equal `peek_depth` and `peek_color` at that
  point and never share a slot with draw-sync snapshots. A slot reused at the other resolution is made anew.
- A snapshot taken with an `EfbRegion` retains the whole EFB on the GPU but reads back only that rectangle. A peek
  outside it waits for the readback and the queue, then reads the retained snapshot, so it stays exact
  (`GlesStats::region_misses`).
- `wait_snapshot` waits for a snapshot's readback without reading a pixel, for a port that delivers a token once
  its snapshot is ready. It waits for the GPU work up to the snapshot, not for GL work queued after it: the GL
  thread publishes readbacks after each task and, while idle, every millisecond, flushing first (an idle thread's
  driver may hold the commands).
- Live depth peeks with no EFB write between them share one readback of the whole EFB's depth (Resident Evil 4's
  lens flares and glows test 12 points each). `request_depth_readback` gives the same depth without waiting for
  GL work before it; requests with no EFB write between them share a readback.
- Readbacks into memory (snapshots and materialization) use `glReadPixels`, except on Panfrost. Mesa serves
  `glReadPixels` on the CPU, into a buffer too (`texture_transfer_modes` 0): each call flushes, blits to a linear
  staging copy, waits for the GPU and converts per pixel. There a compute pass stores the texels into a buffer as
  `packUnorm4x8` of the fetched RGBA8 texel, the bytes `glReadPixels` returns, and materialization copies the
  write-combined mapping to cached memory before encoding. On Mali's vendor driver (r20p0) the compute path
  halved Sunshine's frame rate, so it stays on `glReadPixels`.

## Direct submission

### Recording and segments

`GlesRenderer::draw_direct` reads each draw completely during the decoder callback and records it into a segment.
Recording makes no GL calls. Texture uploads, program links (lazily, before a program's first draw), sampler setup,
geometry buffers and copy targets are queued with the segment and run before its draws; deletions of objects the
segment may still use run after it. Records name objects by GL names taken from pools the GL thread refills, and
copy resident geometry's draw parameters, since packing moves entries while earlier segments are queued.

GL submission happens at the next EFB access, presentation, idle boundary or 16 MiB of pending payload. A segment
uploads its vertices, 32-bit indices and transform uniform blocks with one `glBufferData` each and replays the
records with state diffing. Recording computes per-program uniform changes and adjacent raster and texture changes;
submission skips unchanged groups. A skipped unready program invalidates the affected delta chains so its next
submitted draw restores all required state.

### Batching and merged draws

- Adjacent records with identical program, state, uniform blocks, textures, samplers, vertex run and matrix
  latches merge into one ranged draw.
- A merged draw from the decoder (see [GX model](GX-model.md)) goes through the same path as one draw: one
  program, state plan and texture lookup, one conversion of all its runs, and one resident geometry entry keyed by
  the whole span, headers included. It skips the small-draw paths (early clip test, batching) and is culled by its
  bounds and per-cluster bounds. A stripified mesh costs one draw and one cache lookup per frame instead of one
  per strip. `draw_direct` is the only entry that accepts merged draws.
- Draws whose vertex records are byte-identical are skipped: every triangle has zero area on GX and GL. The
  SDK's XF flush primitive is such a draw.
- Fast mode batches draws of at most 64 vertices whose program reads no object-space input (lighting normal,
  emboss, texgen from position, normal, binormal or tangent, latched normal). Normals their layout carries are
  dropped; positions are transformed to eye space on the CPU by the draw's own position matrix, and the four TEV
  registers travel as flat per-vertex attributes of a batched program variant. In that variant unlit channels
  with a register material read it from the vertex colour, so each draw's material colour travels with its
  vertices; vertex colour c holds what channel c read.
  - Consecutive batched draws share one state plan while the values it read are unchanged: BP registers (the konst
    bank in place of E0-E7), texture extents, CI palette generations and the XF viewport. Games rewrite registers
    between draws, so values are compared, not revisions.
  - They share uniform blocks while what the batched program reads is unchanged: its packed rows hold only its
    texgens' rows, and `GXTransform` replaces only the material lanes of register-material channels.
  - Such draws merge into one GL draw. Resident Evil 4 draws each particle as its own unlit quad whose layout
    carries a normal, with a normal matrix and a material colour; particles and layout panes otherwise cost a
    driver draw each.
- Lines and points go through the batched path at any vertex count: GX's setup unit draws them as screen-aligned
  quads, so after the eye transform each point becomes a square of the BP 0x22 point size in EFB pixels and each
  line segment widens by the line width along the screen axis its slope is further from (`expand_lines_points`,
  using the XF projection and viewport scale). Texture offsets enabled per coordinate (BP 0x30 + 2n) are applied to
  the input coordinate through the inverse of its affine texgen; other texgens fail. The quads ignore the cull
  mode, as GX culls triangles only. Lines and points in a program that cannot batch (lighting normals,
  per-vertex matrices, position texgens, exact shading) fail with an error.
- A batched draw with an alpha test uses a variant that keeps failing fragments with alpha 0 instead of
  discarding them (`batched_zero_alpha`) when that cannot change the EFB: blending adds, the source factor is source
  alpha, the destination factor is one or one minus source alpha, constant alpha is off, and neither alpha nor depth
  is written (GL writes depth only while it tests depth). Such a fragment blends to `0 * source + 1 *
  destination`, which every blend unit stores as the destination it read; fragments that pass are unchanged.
  Dropping the test instead, for a reference of 0 or 1, is not exact: a failing fragment keeps an alpha below
  0.5/255 or 1.5/255, and GL only prefers rounding to nearest when it stores the blended value. A variant's options
  follow `batched_program_word` in its words.
- `GlesDirectLimits::geometry_min_vertices` (default 8) sets the smallest indexed draw kept as resident geometry;
  smaller ones stream and are never instanced. Batched regular TEV values travel per vertex.

### Fog per vertex

With `GlesDirectLimits::vertex_fog`, a batched draw whose vertices all have the same eye depth (bit for bit)
computes its fog factor in the vertex stage (`batched_vertex_fog`) and passes it to the fragment as a flat input;
the fragment still mixes the TEV colour with the fog colour after the alpha test. GX projections have no x or y
term in their z and w rows, perspective or orthographic, so such vertices share one clip z and w and one window
depth. The rasterizer then interpolates a constant depth: `gl_FragCoord.z` equals that window depth at every
fragment, and the factor (`fast_fog_glsl`) is the per-fragment one. Range adjustment (a function of the
fragment's x), Z textures (a per-texel depth) and draws with several depths keep per-fragment fog, as do lines and
points. The vertex derives its window depth from its own clip position and `gl_DepthRange`; a GPU whose viewport
transform rounds differently can differ in the last bit of the depth. Off by default until each game's captures
are compared. In Resident Evil 4's village (rooms 100 and 104) only one or two fogged batched draws per frame have
one eye depth; most particle quads span depths, by more than 0.1 % in about half of them.

The fog type is program identity, and so are its projection and range adjustment while fog is on; with fog off
they are ignored, as games leave them set.

### Instancing

In fast mode, consecutive draws of the same resident geometry with the same program, state plan, textures, matrix
latches, `GXTransform` and `GXPost`, whose packed rows differ only in the position and normal matrix, are one
instanced draw (`glDrawElementsInstancedBaseVertex`) of the program's instanced variant. It reads those two matrices
per instance from `GXInstances` (binding 1, six vec4 rows per instance, at most 128 instances) and its texgen rows
from the first draw's block, so each instance computes exactly what its own draw would. Programs with emboss
texgens, per-vertex matrices and clustered geometry draw separately. A scene of repeated models (Pikmin 2's title
made of hundreds of flowers) costs one driver draw per run of identical shapes. Batched draws that qualify for
merging also qualify for instancing.

### Transform uniform blocks

The vertex stage reads three uniform blocks (`generate_transform_glsl`).

- `GXMatrices` holds the matrix rows a draw reads. A draw whose matrix indices are the XF's packs them: its
  position matrix, its normal matrix when the program reads it, then each texgen's rows; its matrix latches name the
  packed rows (the normal matrix in the top byte of the first word). That is at most 480 bytes instead of the 3,376
  of all XF matrices, lights and registers. Vertices with their own matrix indices read the full form: the 64
  position rows, then the 32 normal rows.
- Every `GXMatrices` range is bound at the block's declared 1,536 bytes over whatever follows it, and the segment's
  uniforms are padded to reach the last one.
- `GXTransform` (projection, ambient and material colours, lights, latched vectors) and `GXPost` (post-transform
  rows, declared only by programs that read them) are built from the previous block with only the lanes the program
  reads replaced, and recorded only when that differs from the previous block. Draws that change their matrices
  add only their packed rows; changes to a colour lane or light a program does not read add nothing. Equal
  consecutive `GXMatrices` blocks are recorded once.
- Scalar uniforms are owned by each cached program. Only active locations with changed byte values are uploaded;
  eviction discards the cache with the program. The owned transform buffer is reuploaded only when its complete
  bytes change; it is rebound for every draw.

Draws without normals that light or generate coordinates from them read the normal, binormal and tangent the XF
latched from the last vertex that carried them, as the hardware does. Every draw with normals captures the raw bytes of
its last enabled vertex (`capture_normal`); only a draw that reads the latch converts them (`decode_normal`). The
latch travels in `GXTransform` and its presence is program identity.

### State plans and programs

- Programs use an incremental register hash with exact word comparisons on a hit; `direct_program_words` runs on
  misses. State plans use a bounded 2,048-entry cache of register-group hashes and exact keys, including the konst
  bank, viewport and host texture extents. Regular TEV colours are refreshed from the draw state on a cache hit.
- State plans retain one immutable state index per segment, including returns to an interleaved state. Independent
  bounded raster and texture planning caches compare all consumed input words, so changing an alpha reference or
  texture binding does not replan unrelated raster state. Program eviction invalidates all three caches. CI texture
  resolutions still check palette generations.
- Every draw validates its current vertices, uniforms and canonical shader key. GLSL source is generated only on a
  compiled-program cache miss; a warm cache never bypasses input validation.
- Projective coordinates first use the original-vertex proof. If that is inconclusive, a bounded homogeneous clipper
  checks the surviving polygons; offscreen singularities cannot reject a valid visible projection. Visible zero-Q
  crossings and overflow fail with an error.

### Guard band and viewport

- `GlesDirectLimits::guard_band`: `GuardBand::reject` refuses scissors reaching past the viewport (GX clips at its
  guard band, GL at the viewport). `GuardBand::clip_to_viewport` intersects the scissor with the viewport, which
  matches a port whose renderer clips at the viewport too (Aurora's). A small single-matrix draw `reject` refuses is
  dropped without an error when every corner of its position box has W > 0 and their screen bounds miss the
  scissor: GX writes no pixel outside the scissor.
- `GlesDirectLimits::fractional_viewport`: `FractionalViewport::reject` refuses a viewport off the pixel grid, since
  `glViewport` takes whole pixels. `FractionalViewport::round` moves each edge to the nearest pixel, at most half a
  pixel from where GX maps it (Dolphin's GLES backend also rounds; hardware unverified). Pikmin 2's pod collection
  movie shifts its 608x448 viewport by 47.2 rows.

### Culling

Draws are dropped when all corners of their object-space bounds, under the draw's position matrix and the XF
projection, lie beyond one clip plane (with a small relative margin). Resident single-matrix geometry keeps its
bounds, so such draws also skip state, uniform and texture work. Resident meshes of at least 1024 triangles also
keep bounds per 256-triangle run of their index buffer and draw only the runs that are not beyond a clip plane
(bridging single culled runs). Clip-disabled draws and per-vertex-matrix geometry are never culled. A culled draw's
program is still handed to the builder after its segment's draws, so a model entering the view at a screen edge has
its program ready instead of staying missing while it builds.

### Textures

- Textures are cached by address, image word, level count, TLUT content and CMPR model and uploaded as immutable
  RGBA8 levels; sampling state uses sampler objects. Each entry's source bytes are content-hashed once per frame, or
  on every use with `verify_textures`, and after `invalidate` reports overlapping CPU writes. Source addresses alone
  never validate cached pixels; neither cache assumes an address is an immutable resource generation.
- The renderer retains textures by owned RGBA content, dimensions and admitted sampler state, so changing map
  slots does not force another upload. LRU eviction protects every texture used by the current draw. Texture bytes
  and object counts are bounded separately; program retention is also capped. These limits exclude driver
  allocations, in-flight allocations and CPU packet storage.
- The `draw(state, command, memory)` entry owns an owner-thread decoding cache that compares current
  source and palette bytes before reusing immutable mip levels. A memory reader may expose a bounds-checked
  canonical span for that synchronous comparison, avoiding a temporary image on a hit. Neither snapshots nor the
  cache retain borrowed spans; misses retain owned source bytes. A per-unit last-hit hint changes lookup order
  only; every candidate must match the full content key. Sampler state and palette generations are captured afresh.
  The cache uses the renderer's texture byte and entry limits as a separate CPU budget (32 MiB/128 by default),
  including retained metadata, source bytes and decoded levels; packets may separately retain evicted data.
  Measured on SP: 120 original strap-layout draws cost 27.1-27.5 ms/frame without decoding reuse and
  2.9-3.2 ms/frame with it (1,347,584 bytes retained); this excludes GPU work, swap and simulation.

### Resident geometry

Converted indexed geometry stays resident on the GPU. It is reused only while the FIFO bytes match exactly and
every consumed array span is unchanged.

- A draw's FIFO bytes match without reading them when `MemoryReader::source_identity` names the same source at
  the same write generation as the entry: a display list read in place, or one the host copied into its FIFO and
  named with the write stamp it had when copied.
- An entry is named by its source, geometry key and payload size, so a list drawn from several vertex arrays finds
  the entry for its arrays in one lookup.
- A source whose new entries go unused until its next insert, twice in a row, is streamed and retried every 64
  frames. Resident Evil 4 draws skinned models from arrays the CPU rewrites at another address every frame;
  caching those inserted about 25 entries a frame that never hit.
- Without a source name, an entry is found by two 64-bit hashes of the payload (and its key) and the payload size.
  The entry's own copy of the payload and key is compared once every 64 frames per entry, like the spans.
- While `MemoryReader::write_epoch` holds a nonzero value, an entry whose spans were all found unchanged is
  not checked again, and `array_view` results are reused. A reader that returns zero has every draw checked.
- A span whose source reports no CPU write since conversion (the game's cache store or flush, reported through
  `MemoryArrayView::generation`) is accepted, and every entry is also compared byte for byte against a retained
  copy once every 64 frames. A span found changed without a reported write marks its array volatile, compared on
  every use from then on; identities whose sources keep changing are streamed instead.
- New entries get their own buffer. At a frame boundary, settled ones are packed into a fresh exact-size arena
  that shares one vertex array per stream format and is drawn by base vertex. Mali drivers pay per draw for every
  live buffer and vertex array object, and copy a whole buffer when submitted draws still use it, so no arena is
  written after its first draw.
- Streamed triangles keep a separate CPU cache of converted columns, capped at 16 MiB or 4,096 entries. The key
  excludes array bases: moved arrays update only columns whose consumed bytes changed. Equal FIFO tuples share a
  converted vertex; different indices remain distinct even when their current values are equal. Unchanged
  generations avoid reads, with periodic byte comparisons as above. Missing source identities take the uncached
  path. `STARLET_VERIFY_STREAM=1` compares every referenced vertex against a fresh conversion before submission.
- Entries of at least eight vertices keep their other attributes and indices in GPU pages and upload only changed
  positions and normals with their segment. Both vertex bindings start on 64-byte boundaries: Mali-G31 r20p0
  fetches wrong attributes for some draws whose buffer offsets do not (black patches on Resident Evil 4's Leon).

### Programs: cache, builder and lists

- With `GlesDirectLimits::program_cache` set, linked program binaries are kept on disk (`GlesProgramCache`), one
  file per program keyed by the driver's vendor, renderer and version strings and the exact GLSL sources, which
  the file stores and compares byte for byte. A checksum rejects torn writes before the driver sees them; a binary
  the driver refuses is deleted and the program is compiled again. Entries are written to a temporary name and
  renamed; above 4096 entries the least recently written are removed down to 3072.
- With fast shading, the words of each linked program are appended to `programs.list` in that directory
  (`ProgramList`). `direct_program_state` rebuilds a State whose words match, so lists stay valid across
  shader-generator changes and devices.
- `GlesProgramBuilder` compiles listed programs the cache lacks on two threads, each with its own surfaceless or
  pbuffer EGL context in the renderer's share group, while the application shows progress before starting the
  game: on Mali, compiles beside the running game stall its GL thread. After `build_programs_in_background()`, one
  low-priority thread builds the rest and the programs the GL thread finds missing; draws using such a program are
  skipped until its binary can be loaded.
- A draw whose program the builder has not delivered is skipped. Once its program has waited
  `GlesDirectLimits::unready_link_ms` (default 67) since its request and no worker has taken the request, the GL
  thread takes it back and links the program itself: one frame lengthened by the link instead of a longer gap. A
  program a worker already compiles stays with the worker: a second compile on the GL thread would end no sooner.
  The GL thread links again only `unready_link_interval_ms` (default 500) after its previous such link ended,
  because a scene that needs many new programs at once would otherwise stall for all their links; those draws wait
  for the builder.
- Measured on muos_35-h (H700, Mali-G31 r20): a GL-thread link takes 76-422 ms beside a busy game. Pikmin 2 from an
  empty cache without a program list, landing in Awakening Wood (200 s): 48,484 draws skipped without the GL-thread
  link, 33,054 with 67/500, 28,085 with 50/250, and 10,805 with 50/250 plus `builder_policy.request_workers` 2 at
  `normal_priority`; the longest frame stayed within 469-544 ms. Without the interval (50/0) only 996 were skipped,
  but the landing linked 43 programs back to back in one 5.5 s frame. Program lists remove these builds; this
  bounds what lists miss. These Pikmin 2 counts predate the rule that leaves a started build to its worker.
- The Wind Waker on muos_35-sp (H700, Mali-G31 r20), empty cache, title to Outset (50/250, two request workers at
  normal priority): builds took 28-230 ms, and about two thirds of the GL thread's links repeated a build already in
  flight. Leaving those to the workers cut the GL thread's links from 23-25 to 10 per run and frames over 100 ms
  from 33-36 to 24, with as many draws skipped (1018-1143 before, 1115-1252 after).
- `tools/replay-all.sh` replays every recorded trace with an empty program cache and the builder running, and fails
  when a program stays undelivered or the builder's books are inconsistent (docs/Replay-traces.md, Cold program cache).

### Batch entry (packets)

`draw_batch` accepts up to 4096 owned packets, synchronously. Every packet is validated before driver mutation;
invalid packets reject the whole batch. Batches of at least 64 packets prepare CPU plans on the owner plus at most
two persistent workers, bounded by reported CPU concurrency; workers read owned immutable snapshots and issue no
GLES commands, and all results and exceptions are joined before driver mutation. Only adjacent packets with
identical shader, raster, uniform and sampled image state share a submission. Geometry keeps its original values and
triangle order; indices are offset into at most 65,535 vertices and 196,605 indices per submission. No work remains
queued after return, so callers keep copy, clear, completion and presentation boundaries explicit. `draws` counts
nonempty input packets and `submissions` counts indexed driver draws. Temporary merged geometry is at most
9,043,830 bytes plus bounded planning data.

Mali-G31 measurements: 188 compatible packets of a recorded file-menu segment became one submission with identical
replayed colour bytes; median warm preparation, submission and completion was 22.17 ms serial and 16.25 ms parallel
(about 27 % less, eight alternating trials, 160-packet batches). Capture, simulation and readback are excluded; this
is not a gameplay benchmark. Cached scalar-uniform uploads gave identical pixels in 18 cases (21,012,480 RGBA
bytes); a repeated 256-draw fixture reduced buffer uploads from 768 to 512.

## Stats

- `GlesStats::draws` counts nonempty input packets; `submissions` counts indexed driver draws.
- `GlesStats::peeks` counts all EFB peeks; `live_peeks` and `peek_waits` those that waited for queued GL work;
  `refused_snapshots` requests refused for want of a slot.
- With a submission thread running, `program_builder()` counters are read unsynchronized.
- `GlesDirectStats::program_waits` counts programs whose draws were skipped by how long they waited since the
  request (within 33, 67, 133, 267 ms, longer); `fallback_links` and `fallback_link_max_ms` count the GL thread's
  own links.
