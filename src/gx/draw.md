# GLES draw integration

The current domain is orthographic/perspective triangles, RGB8 fixed-function
blending, and RGBA6 copy/blend/subtract arithmetic, with linear D24 depth.
Capture validates owned vertex, transform, TEV, texture, raster and pixel
snapshots at the same state revision. Unsupported texture-derived depth, fog,
sampling and logic cases fail with an error.

The renderer's bounded vertex cache shares immutable geometry across captures.
Hits compare current command and array bytes, including source encoding and
implicit matrix selections; per-draw revisions and state remain separate.
Unavailable scoped array views bypass caching. Caller-owned snapshots can outlive
eviction, so cache payload and queued-packet retention have separate budgets.

Lighting channels read vertex colors in the order the vertex supplies them: XF
channel 0 takes CP color 1 when color 0 is absent, and channel 1 takes color 1
only beside color 0. A channel with no vertex color reads white (Deep Dark's
star select draws one), following Dolphin's default for this case.

GX front faces are clockwise on screen. The SDK swaps its public cull-mode
bits into BP hardware encoding; the renderer maps that encoding to GLES face
rejection with clockwise front faces. Culling disables the full-replacement
shortcut, since a rejected primitive cannot replace existing EFB storage.

RGBA6 blending uses coherent ARM or EXT framebuffer fetch, discovered from the
current context. A missing extension rejects destination-dependent draws before
GL mutation. The shader fetches after TEV alpha comparison, converts expanded
RGBA8 storage back to six-bit codes, and applies the existing integer pixel
arithmetic. GL blending stays disabled. The shader identity includes the fetch
dialect. Destination-dependent draws cannot qualify as full EFB replacements.

Coherent fetch preserves API primitive order, including overlapping triangles
inside one draw. The contracts are the Khronos extension specifications:
- https://registry.khronos.org/OpenGL/extensions/ARM/ARM_shader_framebuffer_fetch.txt
- https://registry.khronos.org/OpenGL/extensions/EXT/EXT_shader_framebuffer_fetch.txt

Mali-G31 validation checks all64 factor pairs, subtract, write masks, forced
alpha and dithering, including overlapping quads in one draw. This checks GPU
agreement with the candidate pixel arithmetic; original-Wii numerical
precision remains unverified. EXT shader generation has host coverage; the SP
runtime exercises ARM fetch.

Depth comparison and update masks map to the corresponding GLES state. Native
viewport Z units convert to a normalized near/far range. Early Z uses a fragment-test qualifier, so alpha rejection preserves the configured depth-write
ordering. The shader identity includes that ordering; depth-tested draws cannot
use the full EFB replacement shortcut. Active Z-texture effects remain rejected
when they could change comparisons or writes; read-only ALWAYS/NEVER tests do
not depend on the incoming Z value.

The GX manual sections9.9/9.10/12.2 define the ordering and disabled-write
contracts. Mali validation covers all eight comparisons, depth-write masks,
disabled comparison with update enabled, and alpha rejection before/after Z.
These tests cover those behaviors.

Indirect lookups now own their texture images alongside regular lookups, within
one draw budget. Static two-row matrices decode signed11 coefficients and the
five-bit exponent (the third column's upper scale bit, bit 23, is ignored, as
Dolphin's register layout has it); matrix values stay uniforms. Lookup routing, bias and coordinate
downshifts form shader identity. The implemented domain uses eight-bit ABG
samples, optional per-component signed bias, and S17.7 offset arithmetic.
Wrap modes apply to the stage's regular S17.7 coordinate before the offset is
added: modulo 256, 128, 64, 32 or 16 texels, or zero (GX_ITW_0, which the
depth-of-field blur uses to look up a ramp by depth alone). Feedback, dynamic
matrices and bump-alpha modes fail with an error.
The ordinary sampler filters apply to the indirect image too, and one map may serve
regular and indirect lookups in the same draw. Matrix-off stages without wrapping,
feedback, LOD changes or bump alpha remain direct lookups regardless of their
unused indirect selector, format and bias bits; matrix-off stages that wrap take
the S17.7 path with a zero offset. A stage selecting an indirect stage beyond the
enabled count is undefined on the console; it adds no offset and keeps its wraps.

The offset model truncates the matrix dot product to seven fractional bits
before applying its exponent, then wraps the sum to signed24. This is a
candidate model; Register layout and routing follow Petari's primary GXBump
implementation and the GX manual section10. No reference-port implementation is
included.

Perspective draws use the existing homogeneous transform: W=-eyeZ and GX's
[-W,0] depth interval maps to GLES [-W,W]. GLES clips before the perspective
divide and interpolates texture varyings with reciprocal W. Mali validation
checks varying-W quads against screen-space barycentric sample predictions,
near/far clipping and depth occlusion. The same candidate transform precision
limits apply; this is not full game-image validation.

Mipmapped images upload every captured level. Cache identity and byte limits
cover the full chain, including lower-level mutations. The shader computes an
explicit floating candidate LOD from texel-space derivatives: the longer edge,
or the longer diagonal when requested, followed by signed bias and min/max
clamps. GLES supplies nearest-level or trilinear filtering with the matching
spatial filter. Anisotropy, bias clamp and differing min/mag spatial filters
remain rejected. Planar images retain their single-level sampling behavior.
This preserves mip content and the requested filtering controls; exact original
GX derivative and LOD quantization remain unverified.

CMPR decoding requires an explicit arithmetic model. The GX candidate uses
expanded RGB565 endpoints with eighth-weight opaque interpolation. Transparent
selectors retain the average endpoint RGB with alpha zero, so filtered cutout
edges carry color. The earlier PC S3TC candidate remains separate. Default
capture still rejects CMPR until its caller selects a model.

GXPeekZ flushes the native command stream and synchronously reads the requested
D24 EFB pixel. The GLES depth attachment is a texture; a one-pixel shader packs
the sampled code into a color readback, avoiding GLES depth-read format limits.
GXPeekARGB reads the stored RGBA8 colour directly. Draw-sync snapshots pack both
at quarter resolution with fast shading: each row holds the packed depth, then
the colour. Coordinates convert from native top-left to GLES bottom-left. Driver
work runs in host allocation scope, including calls from the game's draw-sync
callback. Invalid destinations and missing backends fail without publishing a
result.
