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

Converted indexed geometry stays resident on the GPU. It is reused only while
the FIFO bytes match exactly and every consumed array span is unchanged: a span
whose source reports no CPU write since conversion (the game's cache store or
flush, reported through `MemoryArrayView::generation`) is accepted, and every entry is also compared byte
for byte against a retained copy once every 64 frames. A span found changed
without a reported write marks its array volatile, compared on every use from
then on; identities whose sources keep changing are streamed instead. New
entries get their own buffer; at a frame boundary, settled ones are packed into
a fresh exact-size arena that shares one vertex array per stream format and is
drawn by base vertex. Mali drivers pay per draw for every live buffer and vertex
array object, and copy a whole buffer when submitted draws still use it, so no
arena is written after its first draw.

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

`DirectShading::fast` replaces the candidate TEV and pixel arithmetic with the
mediump fragment model of fast_shader.hpp and GL blending: RGBA6 is stored as
RGBA8, blended in all four channels with GX factors (destination alpha included)
and never reinterpreted on pixel-format changes; samplers carry the GX LOD
range. Constant alpha replaces only the stored alpha; colour still blends with
the TEV alpha (raster.hpp, premultiply). It targets visual fidelity, not GX
bit equality.

In fast mode, draws of at most 64 vertices whose program reads no normals and
no position-sourced texgens are batched: their positions are transformed to eye
space on the CPU by the draw's own position matrix, and the four TEV registers
travel as flat per-vertex attributes of a batched program variant. Consecutive
batched draws whose other BP state (`State::bp_state_revision`), XF registers,
textures and remaining XF matrix memory are unchanged share one state plan and
uniform block, and merge into one GL draw; particles and layout panes otherwise
cost a driver draw each.

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

EFB texture copies stay GPU-resident: a shader writes the texels the copy's
sample format decodes (`decode_texture_copy`); in fast shading, copies other
than luminance and RGB5A3 are framebuffer blits whose target swizzle selects
the decoded channels, without the 4/5/6-bit quantization. A later lookup at the same
address with that format, size, one level and natural tile stride samples the
copy directly; any other overlapping lookup first materializes it with a
readback and `store_texture_copy`, reproducing the CPU encoder's bytes.
Unregistered writable ranges release their copies. Display copies render YUYV
into a per-XFB texture that VI presentation samples, so XFB memory is not
written. Because no CPU-visible GX result remains pending, `wait_until_idle`
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
snapshots are read back asynchronously there, so draw-sync peeks read CPU
memory; live peeks and materialization wait for the queue.

These are recording-thread budgets (textures, programs, geometry) of GL objects,
excluding driver allocations.
