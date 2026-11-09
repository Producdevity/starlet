# GX raster capture

`capture_raster` owns the draw's viewport, clipped scissor, SU coordinate
scales, pixel/depth/blend controls, field state and fog registers. The quad
offset is subtracted from both viewport and scissor modulo 2048: a pixel e is
drawn when (e + offset) mod 2048 lies inside the scissor.
`MR::drawFullScreenBlur` relies on this, drawing at y - 1584 to reach EFB row
464 below the 456-line frame. The default offset never reaches the wrapped
interval; a scissor reaching both sides of the wrap fails with an error. A
scissor wholly in EFB rows 456..527 marks a strip draw, planned in that 72-row
target's coordinates; texture copies wholly inside those rows read the same
target (unfiltered colour copies on the direct renderer only). GX inclusive
scissor endpoints become a half-open lower-left rectangle. The viewport is not
clipped or rounded.

`plan_gles_raster` is the initial GPU integration domain, not complete GX support.
It admits integral viewports, scissor contained inside
the viewport, smooth unculled RGB8 triangles and ordinary blending. Depth,
interlace, fog, logic operations, reduced EFB formats and other unsupported
settings fail with an error. The draw layer also validates topology and sampling.

`plan_gles_raster_fast` admits fog: BP 0xEE..0xF1 decode as written by the SDK's
GXSetFog (A and C as 20-bit floats, B as a 24-bit magnitude over 2^24 with a
shift, fsel and projection in 0xF1), the color from 0xF2, and the range
adjustment center (0xE8, minus 342) and its ten 8.8 factors (0xE9..0xED). The
fast fragment program evaluates x = A/(B - z*2^-shift) - C, or A*z - C for
orthographic fog, with z the window depth; with range adjustment the A term is
scaled by the factor interpolated between the samples, which lie (i+1)*32 pixels
from the center column as GXInitFogAdjTable places them, from 1 at the center to
the last sample beyond 320 pixels. x
is clamped to [0, 1] and shaped by the linear, exp, exp2 or reverse curves, then
mixes the color toward the fog color before GL blending; alpha is unchanged.
Fog types 1 and 3 are rejected. The exact domain still rejects all fog.
The GPU sink must enforce `GL_MAX_VIEWPORT_DIMS` to prevent silent driver clamping.

Viewport depth keeps its original units. The RGB8 backing has opaque alpha with
alpha writes disabled. The SDK alpha-update flag is preserved in the snapshot
but has no effect in RGB8, which has no alpha plane; it must not prevent an
otherwise valid draw. Dithering also has no effect in RGB8 ([GX manual, section
11.4](https://www.davidgf.net/downloads/gcwii/gx.pdf)); the GLES dither state
remains disabled. Framebuffer copies, clears, tokens and other BP side effects
are outside capture and must be handled or rejected by the command sink when
written.
