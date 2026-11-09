# GX pixel color arithmetic

`capture_pixel` owns BP00/41/42/43 and their revision. It validates only the color
operation, producing an immutable plan and uniforms. It does not admit a draw,
interpret depth/fog/Z-texture, perform alpha tests, or change renderer gates.
`evaluate_pixel` consumes RGBA8 source, canonical destination codes, EFB x/y,
and the caller's color-acceptance result. A rejected color or masked channel
preserves the old code exactly.

RGBA6 destinations contain four integers 0..63. RGB8 contains three bytes and an
opaque compatibility alpha byte 255. A format change requires a separately
specified conversion or clear; this module never reinterprets existing storage.
Coordinates are within 640x456, x right/y down. In the current lower-left GLES
EFB, use `x=floor(gl_FragCoord.x)`, `y=455-floor(gl_FragCoord.y)`. Presentation
coordinates and viewport-relative coordinates are inappropriate.

## Evidence and candidates

The [Nintendo GX manual v1.03](https://pokeacer.xyz/wii/pdf/GX.pdf), printed
pp124–127, defines normalized coefficients, separately rounded products,
addition/clamping, destination-minus-source subtraction, logic functions,
forced alpha, masks and post-blend RGB dithering. Its six-bit dither arithmetic
is retained directly:

```
Q6 = (((c - (c >> 6)) << 2) + B[y & 3][x & 3]) >> 4
B = {{0,8,2,10}, {12,4,14,6}, {3,11,1,9}, {15,7,13,5}}
f9 = f + (f >> 7)
```

The [Nintendo GXSetBlendMode API tables in US6636214B1](https://patents.google.com/patent/US6636214B1/en)
distinguish source-slot destination-color factors from destination-slot
source-color factors. Pinned Petari `GXEnum.h`, `GXPixel.c` and `private/pe_reg.h`
provide their numeric aliases and BP fields. Original source alpha remains the
factor input when forced alpha replaces the final stored alpha.

`PixelPolicy::documented_only` is the default. It rejects operations needing any
of these unresolved choices. `candidate_v1` selects:

| Assumption bit | Candidate arithmetic |
| --- | --- |
| `product_ties_up` | Round each product as `(c*f9+128)>>8`, then add and saturate. |
| `destination_msb_expansion` | Expand a stored code as `(q<<2)\|(q>>4)` before blending/subtraction. |
| `alpha_truncation` | Reduce final or forced alpha with `a>>2`; RGB dither never changes alpha. |
| `nondither_rgb_truncation` | Reduce nondithered RGB with `c>>2`. |
| `subtract_saturation` | Apply destination-minus-source to each written component, including alpha, with a zero lower clamp. |

A captured snapshot reports every required assumption. The manual does not
settle the product tie convention, internal destination expansion, alpha
dithering or reduction, or the nondithered reduction rule.

Supported arithmetic: RGB8/RGBA6 copy, blend and subtract; all eight factor codes;
independent RGB/alpha masks; RGBA6 forced alpha; sixteen RGB8 logic functions.
Logic/blend conflicts, RGBA6 logic width/order, RGB8 alpha-plane effects, RGB8
dither, RGB565/YUV, multisampling and reserved control bits are rejected. Depth
fields in BP43 are retained but do not influence this color-only evaluator.
No arbitrary unsupported state is treated as a successful no-op.

## GLSL and later renderer integration

`pixel_glsl()` returns a pure GLES3.1 integer helper with no main function,
attachment, framebuffer fetch, depth operation or GL state. Its entrypoint is:

```glsl
uvec4 gx_pixel(uvec4 source, uvec4 destination, ivec2 efb,
              bool color_accepted, uvec4 controls);
```

Both color arguments and coordinates use the CPU valid-input contract. Upload
`PixelSnapshot::uniforms()` as controls: BP41, BP42, format0/1, arithmetic
version1. The helper source is identical for all accepted plans; controls are
per-draw uniforms. Include `pixel_glsl_version` in a future containing program's
key. Capture rejects unproven arithmetic unless requested before the
shader is composed. The helper assumes those validated controls and makes no
admission decisions itself.

A future fetch renderer may encode canonical RGBA6 codes in RGBA8 UNORM bytes
as `q/255.0`, recover them before this function, and store its output the same way.
That encoding requires corresponding clear, readback, copy and presentation
behavior: directly presenting codes would look dark. Fixed-function blending,
GL dithering, sRGB conversion and MSAA must not alter an integer color-stage
result. Framebuffer-fetch ordering and UNORM round trips require separate device
validation; this module does not implement them.

`starlet_gx_pixel_tests --fixtures PREFIX` writes a compute shader and 4,880
little-endian test cases. Each `.cases` record contains sixteen u32 values:
source RGBA, destination RGBA, controls, then x/y/accepted/reserved. Each `.expected`
record is four u32 output codes. The shader accepts input/output SSBO bindings0/1,
`uCount`, and 64 invocations per group. The vectors are bounded development
artifacts, not assets or renderer code. Host tests also exhaust all 65,536
color/coefficient combinations, 1,048,576 logic cases, every dither input/phase,
factor pairs, masks, rejection and forced-alpha ordering.

RGB8 has no stored alpha plane. Its alpha-update bit is retained in captured
words but the effective plan disables alpha writes, preserving opaque backing
alpha. Other unadmitted RGB8 pixel controls still fail with an error.
