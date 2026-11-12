# Direct GLES submission

`GlesRenderer::draw_direct` reads each draw completely during the decoder
callback and records it into a segment; GL submission happens at the next EFB
access, presentation, idle boundary or 16 MiB of pending payload. A segment
uploads its vertices, 32-bit indices and transform uniform blocks with one
`glBufferData` each and replays the records with state diffing. Adjacent records
with identical program, state, uniform blocks, textures, samplers, vertex run and
matrix latches merge into one ranged draw.

Programs are cached by `direct_program_words` (direct.md). Per-draw state is
replanned when BP or XF-register revisions change. Draws whose vertex records
are byte-identical are skipped: every triangle has zero area on GX and GL. The
SDK's XF flush primitive is such a draw.

Textures are cached by address, image word, level count, TLUT content and CMPR
model and uploaded as immutable RGBA8 levels; sampling state uses sampler
objects. Each entry's source bytes are content-hashed once per frame, or on
every use with `verify_textures`, and after `invalidate` reports overlapping CPU
writes.

Draws without normals that light or generate coordinates from them read the
normal, binormal and tangent the XF latched from the last vertex that carried
them, as the hardware does; every draw with normals captures the raw bytes of its
last enabled vertex (`capture_normal`) and only a draw that reads the latch converts them
(`decode_normal`). The latch travels in `GXTransform` and its presence is program identity.

The vertex stage reads three uniform blocks (`generate_transform_glsl`). `GXMatrices`
holds the matrix rows a draw reads. A draw whose matrix indices are the XF's packs them:
its position matrix, its normal matrix when the program reads it, then each texgen's rows,
and its matrix latches name the packed rows (the normal matrix in the top byte of the
first word). That is at most 480 bytes instead of the 3,376 of all XF matrices, lights and
registers. Vertices with their own matrix indices read the full form: the 64 position
rows, then the 32 normal rows. Every `GXMatrices` range is bound at the block's declared
1,536 bytes over whatever follows it, and the segment's uniforms are padded to reach the
last one. `GXTransform` (projection, ambient and material colours, lights, latched
vectors) and `GXPost` (post-transform rows, declared only by programs that read them) are
built from the previous block with only the lanes the program reads replaced, and recorded
only when that differs from the previous block. Draws that change their matrices then add
only their packed rows, and changes to a colour lane or light a program does not read add
nothing. Equal consecutive `GXMatrices` blocks are recorded once.

Converted indexed geometry stays resident on the GPU. It is reused only while the
FIFO bytes match exactly and every consumed array span is unchanged. A draw's FIFO
bytes match without reading them when `MemoryReader::source_identity` names the same
source at the same write generation as the entry: a display list read in place, or
one the host copied into its FIFO and named with the write stamp it had when copied.
An entry is named by its source, geometry key and payload size, so a list drawn from
several vertex arrays finds the entry for its arrays in one lookup. A source whose new
entries go unused until its next insert, twice in a row, is streamed and retried every 64
frames: Resident Evil 4 draws skinned models from arrays the CPU rewrites at another
address every frame, and caching those inserted about 25 entries a frame that never hit.
A span whose source reports no CPU write since conversion (the game's cache store or
flush, reported through `MemoryArrayView::generation`) is accepted, and every entry
is also compared byte for byte against a retained copy once every 64 frames. A span
found changed without a reported write marks its array volatile, compared on every
use from then on; identities whose sources keep changing are streamed instead. New
entries get their own buffer; at a frame boundary, settled ones are packed into a
fresh exact-size arena that shares one vertex array per stream format and is drawn
by base vertex. Mali drivers pay per draw for every live buffer and vertex array
object, and copy a whole buffer when submitted draws still use it, so no arena is
written after its first draw.

A merged draw (decoder.md) goes through the same path as one draw: one program,
state plan and texture lookup, one conversion of all its runs, and one resident
geometry entry keyed by the whole span, headers included. It skips the small-draw
paths (early clip test, batching) and is culled by its bounds and per-cluster bounds
instead. A stripified mesh then costs one draw and one cache lookup per frame
instead of one per strip.

`GlesDirectLimits::guard_band` selects how scissors reaching past the viewport are
handled. `GuardBand::reject` refuses them, as GX clips at its guard band and GL at
the viewport. `GuardBand::clip_to_viewport` intersects the scissor with the
viewport, which matches a port whose renderer clips at the viewport too (Aurora's).

`GlesDirectLimits::fractional_viewport` selects how a viewport off the pixel grid is
handled. `FractionalViewport::reject` refuses it, since `glViewport` takes whole
pixels. `FractionalViewport::round` moves each edge to the nearest pixel, at most half
a pixel from where GX maps it (Dolphin's GLES backend also rounds); Pikmin 2's pod
collection movie shifts its 608x448 viewport by 47.2 rows.

`DirectShading::fast` replaces the candidate TEV and pixel arithmetic with the
mediump fragment model of fast_shader.hpp and GL blending: RGBA6 is stored as
RGBA8, blended in all four channels with GX factors (destination alpha included)
and never reinterpreted on pixel-format changes, by draws, copies or clears
alike (an SDK depth copy selects Z24 inside RGBA6 frames); samplers carry the GX LOD
range. Constant alpha replaces only the stored alpha; colour still blends with
the TEV alpha (raster.hpp, premultiply). The fog type is program identity, and so
are its projection and range adjustment while fog is on; with fog off they are
ignored, as games leave them set. It targets visual fidelity, not GX bit equality.

In fast mode, draws of at most 64 vertices whose program reads no object-space input
(lighting normal, emboss, texgen from position, normal, binormal or tangent, latched
normal) are batched, and normals their layout carries are dropped: their positions are
transformed to eye space on the CPU by the draw's own position matrix, and the four TEV
registers travel as flat per-vertex attributes of a batched program variant. In that
variant unlit channels with a register material read it from the vertex colour, so each
draw's material colour travels with its vertices; vertex colour c then holds what
channel c read. Consecutive batched draws share one state plan while the values it read
are unchanged: BP registers (the konst bank in place of E0-E7), texture extents, CI
palette generations and the XF viewport. Games rewrite registers between draws, so values are compared, not revisions.
They share their uniform blocks while what the batched program reads is unchanged: its
packed rows hold only its texgens' rows, and `GXTransform` replaces only the material
lanes of register-material channels. Such draws merge into one GL draw; Resident Evil 4
draws each particle as its own unlit quad whose layout carries a normal, with a normal
matrix and a material colour, and particles and layout panes otherwise cost a driver
draw each.

A batched draw with an alpha test uses a variant that keeps failing fragments with alpha 0
instead of discarding them (`batched_zero_alpha`) when that cannot change the EFB: blending
adds, the source factor is source alpha, the destination factor is one or one minus source
alpha, constant alpha is off, and neither alpha nor depth is written (GL writes depth only
while it tests depth). Such a fragment blends to 0 * source + 1 * destination, which every
blend unit stores as the destination it read; fragments that pass are unchanged. Dropping
the test instead, for a reference of 0 or 1, is not exact: a failing fragment keeps an alpha
below 0.5/255 or 1.5/255, and GL only prefers rounding to nearest when it stores the blended
value. A variant's options follow `batched_program_word` in its words.

With `GlesDirectLimits::vertex_fog`, a batched draw whose vertices all have the same eye
depth (bit for bit) computes its fog factor in the vertex stage (`batched_vertex_fog`) and
passes it to the fragment as a flat input; the fragment still mixes the TEV colour with the
fog colour after the alpha test, as before. GX projections have no x or y term in their z and
w rows, perspective or orthographic, so such vertices share one clip z and w and one window
depth. The rasterizer then interpolates a constant depth: `gl_FragCoord.z` equals that
window depth at every fragment, and the factor, the same expression of it (`fast_fog_glsl`),
is the per-fragment one. Range adjustment (a function of the fragment's x), Z textures (a
per-texel depth) and draws with several depths (the factor is not linear in depth) keep
per-fragment fog, as do lines and points. The vertex derives its window depth from its own
clip position and `gl_DepthRange`; a GPU whose viewport transform rounds differently can
differ in the last bit of the depth. It is off by default until each game's captures are
compared. In Resident Evil 4's village (rooms 100 and 104) only one or two fogged batched
draws per frame have one eye depth; most particle quads span depths, by more than 0.1 % in
about half of them, so they keep per-fragment fog.

In fast mode, consecutive draws of the same resident geometry with the same program,
state plan, textures, matrix latches, `GXTransform` and `GXPost`, whose packed rows
differ only in the position and normal matrix, are one instanced draw
(`glDrawElementsInstancedBaseVertex`) of the program's instanced variant: it reads those
two matrices per instance from `GXInstances` (binding 1, six vec4 rows per instance, at
most 128 instances) and its texgen rows from the first draw's block, so each instance
computes exactly what its own draw would. Programs with emboss texgens, per-vertex
matrices and clustered geometry draw separately. A scene of repeated models (a title made of
hundreds of flowers) then costs one driver draw per run of identical shapes.

Games often write BP registers again with unchanged values between draws (a material
loaded per model). A draw whose BP values equal those of the last state plan keeps that
plan and the textures it resolved, without planning or texture lookups; CI textures also
need their palette generation unchanged, since a TLUT load changes the palette without
changing a register. Such draws then also qualify for instancing.
`GlesDirectLimits::geometry_min_vertices` (default 8) sets the smallest indexed draw kept
as resident geometry; smaller ones stream and are never instanced.

Lines and points go through the batched path at any vertex count: GX's setup
unit draws them as screen-aligned quads, so after the eye transform each point
becomes a square of the BP 0x22 point size in EFB pixels and each line segment
widens by the line width along the screen axis its slope is further from
(`expand_lines_points`, using the XF projection and viewport scale). Texture
offsets enabled per coordinate (BP 0x30 + 2n) are applied to the input
coordinate through the inverse of its affine texgen; other texgens fail. The
quads ignore the cull mode, as GX culls triangles only. Lines and points in a
program that cannot batch (lighting normals, per-vertex matrices, position
texgens, exact shading) fail with an error.

Draws are dropped when all corners of their object-space bounds, under the
draw's position matrix and the XF projection, lie beyond one clip plane (with
a small relative margin); resident single-matrix geometry keeps its bounds, so
such draws also skip state, uniform and texture work. Resident meshes of at
least 1024 triangles also keep bounds per 256-triangle run of their index
buffer, and draw only the runs that are not beyond a clip plane (bridging
single culled runs). Clip-disabled draws and per-vertex-matrix geometry are
never culled. A culled draw's program is still handed to the builder after its
segment's draws, so a model entering the view at a screen edge has its program
ready instead of staying missing while it builds.

A draw whose program the builder has not delivered is skipped. Once its program
has waited `GlesDirectLimits::unready_link_ms` (default 67) since its request, the
GL thread links it itself: one frame lengthened by the link instead of a longer
gap. The GL thread does that again only `unready_link_interval_ms` (default 500)
after its previous such link ended, because a scene that needs many new programs
at once would otherwise stall for all their links; those draws wait for the builder.
On muos_35-h (H700, Mali-G31 r20) a GL-thread link takes 76–422 ms beside a busy
game. Pikmin 2 from an empty cache without a program list, landing in Awakening
Wood (200 s): 48,484 draws skipped without the GL-thread link, 33,054 with 67/500,
28,085 with 50/250, and 10,805 with 50/250 plus `builder_policy.request_workers`
2 at `normal_priority`; the longest frame stayed within 469–544 ms. Without the
interval (50/0) only 996 were skipped, but the landing linked 43 programs back to
back in one 5.5 s frame. Program lists remove these builds; this bounds what lists miss.
`GlesDirectStats::program_waits`, `program_wait_max_ms`, `fallback_links` and
`fallback_link_max_ms` report the waits and links.

EFB texture copies stay GPU-resident: a shader writes the texels the copy's
sample format decodes (`decode_texture_copy`); in fast shading, copies other
than luminance and RGB5A3 are framebuffer blits whose target swizzle selects
the decoded channels, without the 4/5/6-bit quantization. A later lookup at the same
address with that format, size, one level and natural tile stride samples the
copy directly. A smaller lookup whose natural stride is the copy's reads the
copy's top-left texels: it samples a texture of its own size that
`glCopyTexSubImage2D` fills from the copy's framebuffer on the GPU, kept until
the copy is replaced, so its sampler wraps and clamps at the lookup's edges.
Any other overlapping lookup first materializes the copy with a
readback and `store_texture_copy`, reproducing the CPU encoder's bytes.
`release_resident` drops the copies in a range the port no longer lets the GX
write (Galaxy unregisters its writable ranges this way).

Host transfers on the game's behalf call `starlet::os::before_dma_read` or
`before_dma_write` first: ARAM and disc DMA, card reads and writes, THP planes
and audio DMA. gx_fifo passes one whose range holds a byte a resident copy owns
(`resident_in`: each copy's destination and the uncovered ranges of partly
replaced copies, republished by the GPU thread whenever its copies change) to
the GPU thread after the commands queued before it, while the transfer waits.
Other transfers never wait for the GPU thread: they cost an atomic load while
nothing is resident and a locked scan otherwise (about 50 ns on the arm64 test
host). A read materializes the copies it
reaches, waiting for the GL thread's readback, so the transfer gets the copy's
bytes. A write drops the copies it covers without materializing them; one it
covers in part records the written range as covered and is kept like a copy a
newer one partly replaced (below), so its other bytes stay exact. Cache stores
and flushes only change page generations, since games flush memory holding valid
copies before swapping it out. A port whose own DMA bypasses `starlet::os` calls
the renderer's `before_dma_read` and `before_dma_write` itself. Copies by the
game's CPU are not transfers: Resident Evil 4 swaps its sub screen window with
ARAM by `memcpy` through 64 KiB DMA buffers (`MemorySwap`), so only stamps show
that the room's bytes replaced the sub screen's copies there.

Where nobody reports CPU writes, as in a recompiled game or a CPU copy like
that one, `stamp_resident_copies` writes a per-copy stamp into the first eight
bytes of up to 64 destination blocks spread over the copy, its first and last
included. The copy is sampled or materialized only while
every stamp is intact; a game reusing the memory overwrites it wholesale and
breaks one, and the memory then keeps the game's bytes. Stamping every block
cost a cache miss per block, several milliseconds per frame on the H700.

A copy that overlaps part of an older one takes over only those bytes: the older
copy keeps its texture and records the ranges newer copies own (`covered`), and
is retired once they cover all of it. A lookup reaching its remaining bytes
materializes it, writing only those bytes, together with the current copies the
lookup overlaps. This is exact: the texture is never rendered into again, so it
writes what materializing at the overlap would have written, and every lookup
checks older copies before reading memory. Remaining bytes are disjoint from
every other copy's, so materialization order does not matter. Releases and
stamps apply as to any resident copy; stamps in covered ranges are the newer
copies' and are not checked. Beyond eight older copies the oldest is
materialized. Resident Evil 4 writes its 256x196 RGBA8 refraction copy and then
a 256x224, 128x112 and 64x56 I8 downsample chain at the same address, each
covering the start of the previous one; this removes three readbacks per frame.

Display copies render YUYV into a per-XFB texture that VI presentation samples,
so XFB memory is not written. Because no CPU-visible GX result remains pending, `wait_until_idle`
only submits records; presentation still waits for the previous frame.

Recording makes no GL calls. Texture uploads, program links (lazily, before a
program's first draw), sampler setup, geometry buffers and copy targets are
queued with the segment and run before its draws; deletions of objects the
segment may still use run after it. Records name objects by GL names taken
from pools that the GL thread refills, and copy resident geometry's draw
parameters, since packing moves entries while earlier segments are queued.
`Target::execute` decides where segments run: inline, or on the renderer's
submission thread (`GlesRenderer::start_submission`), which also runs EFB
clears, copies, display copies and presentation in call order. EFB
snapshots (quarter resolution under fast shading unless `snapshot_scale` sets
another; callbacks that test single pixels need 1) are read back
asynchronously there, so draw-sync peeks read CPU
memory; live peeks and materialization wait for the queue. A snapshot taken
with an `EfbRegion` retains the whole EFB on the GPU but reads back only that
rectangle; a peek outside it waits for the readback and the queue, then reads
the retained snapshot, so it stays exact (`GlesStats::region_misses`).
`wait_snapshot` waits for a snapshot's readback without reading a pixel, for a
port that delivers a token once its snapshot is ready.

Readbacks into memory (snapshots and materialization) use `glReadPixels`, except
on Panfrost. Its Mesa driver serves `glReadPixels` on the CPU, into a buffer too
(`texture_transfer_modes` 0): each call flushes, blits to a linear staging
copy, waits for the GPU and converts per pixel. There a compute pass stores the
texels into a buffer as `packUnorm4x8` of the fetched RGBA8 texel, the bytes
`glReadPixels` returns, and materialization copies the write-combined mapping
to cached memory before encoding. On Mali's vendor driver (r20p0) the compute
path halved Sunshine's frame rate, so it stays on `glReadPixels`.

These are recording-thread budgets (textures, programs, geometry) of GL objects,
excluding driver allocations.
