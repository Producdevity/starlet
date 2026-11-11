# Direct GLES submission

`GlesRenderer::draw_direct` reads each draw completely during the decoder
callback and records it into a segment; GL submission happens at the next EFB
access, presentation, idle boundary or 16 MiB of pending payload. A segment
uploads its vertices, 32-bit indices and transform uniform blocks with one
`glBufferData` each and replays the records with state diffing. Adjacent records
with identical program, state, uniform block, textures, samplers, vertex run and
matrix latches merge into one ranged draw.

Programs are cached by `direct_program_words` (direct.md). Per-draw state is
replanned when BP or XF-register revisions change; the 3,328-byte transform
block is rebuilt when XF memory or registers change. Draws whose vertex records
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
(`decode_normal`). The latch travels in the transform uniform block and its presence is program identity.

Converted indexed geometry stays resident on the GPU. It is reused only while the
FIFO bytes match exactly and every consumed array span is unchanged. A draw's FIFO
bytes match without reading them when `MemoryReader::source_identity` names the same
source at the same write generation as the entry: a display list read in place, or
one the host copied into its FIFO and named with the write stamp it had when copied.
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

In fast mode, draws of at most 64 vertices whose program reads no normals and
no position-sourced texgens are batched: their positions are transformed to eye
space on the CPU by the draw's own position matrix, and the four TEV registers
travel as flat per-vertex attributes of a batched program variant. Consecutive
batched draws whose other BP state (`State::bp_state_revision`), XF registers,
textures and remaining XF matrix memory are unchanged share one state plan and
uniform block, and merge into one GL draw; particles and layout panes otherwise
cost a driver draw each.

In fast mode, consecutive draws of the same resident geometry with the same program,
state plan, textures and matrix latches, whose transform blocks differ only in the
position matrix rows pn..pn+2 and normal matrix rows pn % 32..+2 of the draw's PN index,
are one instanced draw (`glDrawElementsInstancedBaseVertex`) of the program's instanced
variant: it reads those two matrices per instance from `GXInstances` (binding 1, six
vec4 rows per instance, at most 128 instances) and everything else from the first
draw's block, so each instance computes exactly what its own draw would. Programs with
emboss texgens, texgens reading rows that overlap the PN rows, per-vertex matrices and
clustered geometry draw separately. A scene of repeated models (a title made of
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
copy directly; any other overlapping lookup first materializes it with a
readback and `store_texture_copy`, reproducing the CPU encoder's bytes.
Unregistered writable ranges release their copies. Where nobody reports CPU
writes, as in a recompiled game, `stamp_resident_copies` writes a per-copy stamp
into the first eight bytes of up to 64 destination blocks spread over the copy,
its first and last included. The copy is sampled or materialized only while
every stamp is intact; a game reusing the memory overwrites it wholesale and
breaks one, and the memory then keeps the game's bytes. Stamping every block
cost a cache miss per block, several milliseconds per frame on the H700.

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

These are recording-thread budgets (textures, programs, geometry) of GL objects,
excluding driver allocations.
