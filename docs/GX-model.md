# GX model

`starlet_gx` is the GX command and state model: a C++17 library with no GL, SDL or game dependency. CTest registers the decoder tests as `starlet_gx_core`. This page covers the contract a port relies on, the GX hardware behaviour the model implements, with sources and verification status, and the boundary to the GLES renderer ([GLES-renderer.md](GLES-renderer.md)).

Status words used below: "hardware unverified" means no console capture backs the behaviour. "Dolphin" means behaviour read from the Dolphin source for reference only; no code was imported.

## Sources

Independent implementation against the GX register definitions in the Petari decompilation's SDK (`private/cp_reg.h`, `private/xf_mem.h`, `private/tx_reg.h`, `private/pe_reg.h`) and its command emitters (`GXAttr.c`, `GXTexture.c`, `GXDisplayList.c`, `GXTransform.c`, `GXPixel.c`, `GXBump.c`, `GXFrameBuf.c`, `GXVert.h`, `GXRegs.h`, `GXEnum.h`), plus JUTDirectPrint.cpp for the YUV coefficients. Manuals:

- [Nintendo GX manual v1.00](https://www.davidgf.net/downloads/gcwii/gx.pdf): sections 4.3-4.6, 7 and Appendix C for vertex and display-list state; section 11.4 for RGB8 dithering.
- [GX manual v1.03](https://pokeacer.xyz/wii/pdf/GX.pdf): printed pp103, 122-127 (early/late Z, blend arithmetic, dithering), sections 8.9, 8.10, 9.9, 9.10, 9.12, 10, 11.2, 12.2, 12.5 and Appendix D.
- [Advanced GX](https://pokeacer.xyz/wii/pdf/Advanced_GX.pdf) section 14 (display copy).
- [US 6,636,214 B1](https://patents.google.com/patent/US6636214B1/en): GXSetBlendMode factor tables. US 6,980,218: emboss texgen.

Tests call the implementation with independently specified register values, byte streams and expected snapshots. No predecessor port implementation, shader or test source is used.

## Decoder and MemoryReader contract

The renderer implements `CommandSink`. The port provides `MemoryReader` over registered GX address tokens; no FIFO address is cast to a native pointer.

- `MemoryReader::copy` returns canonical big-endian GX bytes. Native mutable arrays and matrices need a typed view (`MemoryArrayView`, including `host_order`) or a conversion. Copying host little-endian memory into this interface is wrong.
- Display-list contents must stay stable during `MemoryReader::copy`.
- Indexed XF loads (CP 0x20, 0x28, 0x30, 0x38) read through `copy_indexed_xf`, which names the array (12 to 15) and defaults to `copy`. Games compute those matrices and lights on the CPU every frame, so a port whose CPU writes host-order numbers converts them there. The words returned are canonical.
- Host-order arrays hold numbers in host byte order at their own width, as a PC port's native code writes them. They convert like native views of that width.

### Feeding and failure

- `Decoder::feed` takes big-endian FIFO or display-list bytes. `write_u8/u16/u32/f32` replace a port's write-gather writes. All paths mutate the same state in command order.
- `finish` checks a submission boundary for a truncated command. It does not clear state.
- Errors latch. Commands already completed stay applied. Discard a failed decoder.
- An exception from a callback or from decoder allocation also latches before propagating with its original type. The latched failure message is empty so handling an allocation failure does not itself allocate. After catching one, later feeds cannot replay a partially processed command.
- The decoder buffers at most one bounded command, reads every display-list call into a fresh owned snapshot, and limits nesting, expanded input and command count per feed. Nesting has an absolute ceiling of 64. The other limits are configurable.
- Every applied BP write reaches the sink, which must reject operations it cannot implement. The decoder handles CP state, XF writes, the four indexed XF arrays, VCD/VAT vertex sizing, TLUT loads and vertex-cache invalidation. Unsupported opcodes, CP addresses, formats and out-of-range transfers fail with an error.

### Draw callbacks

- Draw callbacks are synchronous. The state and raw vertex bytes are valid only during the callback; indexed vertex arrays and textures must be read or snapshotted there.
- Attribute `components` counts all components per vertex (nine for NBT); `indices` distinguishes one NBT index from three separate ones.
- The renderer owns vertex expansion, reserved position-index skip markers, primitive assembly, conversion and quantization. The decoder supplies no packet queue or GL lifetime. An asynchronous renderer must own all referenced data, retain immutable palette blocks, and retire CPU and GPU resources separately.

### Current matrix indices

A draw with per-vertex matrix indices leaves its last enabled vertex's indices as the current indices, without changing the XF register image. Every write of XF 0x1018/0x1019 restores the indices from the written word, including a write of the value already held. J3D reloads an unchanged current matrix after skinned shapes; skipping it draws the next shape with a skinned joint's matrix.

### Frame layout

`Decoder` takes the `EfbLayout` its state reports: the frame a game's render mode draws, 640 wide and `height` rows of the 528-row EFB (456 for Super Mario Galaxy, 480 for NTSC GameCube modes). Rows below the frame hold scratch work such as blur strips, which renderers keep in a separate target. Planners for rasterization, copies and clears bound their rectangles by the layout. Give every decoder and renderer of one port the same layout.

### TLUT loads

- The BP 0x64 source is bits 0-20 in 32-byte units; the console ignores bits 21-23, which SDKs can leave set from stale register contents.
- Palette offsets are relative to TMEM `0x80000`.
- A load creates immutable blocks and advances generations even if the address and BP command repeat within a frame. Generations cover 512-byte blocks conservatively. A texture cache key must include its palette range generation and format; no cache may treat an unloaded palette range as valid.
- State copies preserve prior palette contents.
- Generations are local to one `Decoder` lifetime and a new decoder may reuse the same numbers. A renderer that replaces its decoder must clear its texture caches or add its own decoder epoch to every palette cache key.

### Merged primitives

With `Limits::merge_primitives`, back-to-back quad, triangle, strip and fan commands of one VAT reach the sink as one `Draw`. No command separates them, so one state draws them all. `Draw::runs` lists each primitive's opcode, vertex count and the offset of its first vertex in `vertices`, which spans every run including the three-byte header before each later one. `primitive` and `vertex_count` describe the first run; `total_vertices()` counts all. Merging stops at any other command, a VAT change, a line or point command, a primitive without vertices, the end of the fed bytes and 65535 vertices. The last enabled vertex of the last run latches the matrix indices.

Display lists strip their meshes into many short primitives. Twilight Princess's field draws about 10000 primitives a frame; merging turns them into about 200 draws. Enable merging only for sinks that read `runs`. `GlesRenderer::draw_direct` does; `capture_vertices` and `GlesRenderer::draw` reject merged draws.

### Host texture extents

GX image registers hold sizes up to 1024 texels. A PC port's own textures can be larger: Twilight Princess's PC build joins a font's pages into one 128x1152 image. `Decoder::set_texture_extent(map, {width, height})` (up to 8192 per side) gives a map that size while its image register holds the extent's low ten bits in each dimension; any other register value ignores the extent. Texture coordinates keep scaling against the register size, so they stay normalized to the whole texture. An extent change counts as a BP change.

### Fast commands

NOP runs, plain BP writes (both TEV banks), plain CP writes and XF writes of up to 16 words are most commands. `fast_command` applies them directly, with the general path's state, revisions and `bp_write` callbacks, at the stream's top level and inside display lists. Pikmin 2 sends most of its material and shape state through lists (about 590 KB of list bytes per title frame). A list's bytes are charged when the list is called, so commands inside it charge only their count. TLUT loads, texture-cache invalidation, masked writes, indexed loads, primitives and host opcodes, or hitting a limit, take the general path. `Decoder::counts()` reports how many commands each path took, by opcode for the general path.

### Host commands

A PC port's FIFO can carry the port's own commands: Aurora's opcode 0x50 names host pointers and stands for the register writes a console made.

- Set their opcodes in `Limits::host_opcodes`. `CommandSink::host_command` receives each at its first byte with the rest of the fed bytes, sets `consumed` to its size and may apply the commands it stands for with `Decoder::inject`. A host command must arrive whole in one feed: give the decoder a drained FIFO buffer in one `feed`, starting at a command boundary.
- `Limits::filtered_bp` names BP addresses whose stream writes pass through `CommandSink::filter_bp` before they apply. The sink may rewrite the value, drop the write, or run the operation itself and inject the writes it stands for.
- `Limits::dropped_bp` names addresses whose stream writes are ignored without a callback, for registers the host always supplies.
- Injected commands are hardware commands: they skip `host_opcodes` and `filter_bp`. A display list an injected `CALL_DL` runs decodes as stream commands, so a host can turn its own list call into a real one.
- `inject` applies whole commands at the current stream position under the running feed's work limits. It works between feeds and from `host_command` and `filter_bp`. From any other callback, or while a partial command is buffered, it fails with `reentrant_call`.

## Vertex loading and capture

- `capture_vertices` snapshots a draw's geometry: immutable expanded attributes and indices plus the effective matrix indices. Absent attributes are zero placeholders, not hardware defaults; the renderer supplies them. A failure publishes no snapshot.
- `append_stream_vertices` converts one draw to an interleaved record holding only present attributes (`StreamFormat`), in the transform shader's input units, plus triangle-list indices. Values are bit-identical to `capture_vertices` for every position, normal/NBT, color, texture-coordinate and matrix-index format, canonical or native-endian array, view or copy source; a randomized host test compares 4,000 layouts.
- Disabled vertices keep zeroed slots. Fully disabled primitives are dropped. Partially disabled primitives, lines and points are rejected. A failure restores both output vectors.
- `append_stream_records` converts a line or point draw the same way, without indices, and rejects index-disabled vertices; the caller expands them (`gx_line_point`).
- Layouts without explicit matrix indices carry no per-vertex matrix words; the renderer supplies `stream_matrix_latches` as a constant attribute. `StreamArraySpans` report each indexed array's consumed byte range so a caller can validate reuse of converted geometry.
- A merged draw converts as one: its runs' vertices are gathered into a contiguous buffer and converted once, and each run emits its own indices against its offset. The result equals converting the runs one by one. Use `append_stream_vertices` for merged draws; `append_stream_records` rejects them.

## Transform and lighting

- The XF numbers vertex colors in the order present: channel 0 reads CP color 1 when color 0 is absent, and channel 1 reads color 1 only beside color 0. A channel with no vertex color reads white (Deep Dark's star select draws one), following Dolphin; hardware unverified.
- A texgen whose TEXn source is absent from the vertex reads (0, 0, 1, 1) (Pikmin 2's Day 1 draws one). Dolphin is the reference; hardware unverified.
- The XF keeps the normal, binormal and tangent of the last vertex it received. A draw without them lights and generates coordinates with those (`TransformUniforms::latched_normal`).
- Numerical model: float32 from the GX equations, not hardware precision. Normals are normalized for lighting; texgen uses raw input. Light accumulation truncates the sum to a signed integer before ambient and clamp; material multiplication stays float until the raster/TEV boundary. No blanket fast-math.
- Games leave unused lights enabled with every attenuation coefficient zero; such a light adds nothing. A zero divisor under a positive numerator saturates the light instead of producing an infinity that a black light would turn into NaN. Games also leave the direction of a distance-only light uninitialized, so the direction matters only where an angle term exists.
- The texgen register holds 18 bits and the post-transform register 9. SDK unmasked field writes can set bits above them; the hardware never sees those bits.
- Clip-disabled draws still clip depth against the GX near/far range; a draw wholly behind the eye or beyond one depth plane writes nothing. X/Y clipping preserves viewport pixels when the scissor lies inside the viewport.
- GLES boundary: GX depth [-W, 0] maps to GLES [-W, W] through `z' = 2z + w`. X/Y are unchanged. The renderer owns viewport Y convention, depth range, scissor and presentation; viewport register values are never baked into clip coordinates.
- Value-dependent preconditions of the vertex shader: normalized vectors need finite squared length above 1e-30, attenuation denominators absolute value above 1e-20, and all intermediates must stay finite. The diagnostic validator checks them; the shader inserts no numerical fallback.
- Performance: the vertex stage writes no `gl_PointSize`. Only triangles are rasterized, and Mali r20 makes every draw of a point-size-writing stage cost about 3x the CPU time in the driver.

### Lines and points

GX's setup unit draws lines and points as screen-aligned quads. `gx_line_point` expands eye-space stream records into triangle-list records. A point covers `size` pixels around its vertex with offsets S, T and S+T at its right, lower and lower-right corners. A line widens by `size` along the screen axis its slope is further from, and the side on the positive axis gets the S offset. BP 0x22 and the per-coordinate enables in BP 0x30 + 2n select the texture offsets. Primitives with a vertex on or behind the eye plane are dropped.

## TEV

- Arithmetic is an explicit provisional model (`fixed8_candidate_v1`), not proof of original precision. CPU and GLSL policy live together in `src/gx/tev.cpp`; CPU/GPU agreement establishes only the model's consistency. Nearest rounding with ties toward +infinity.
- Signed `>>` sign-extends (GLSL ES 3.00 5.9), so it is floor division by 256 or 512.
- The two equations of a stage both consume the incoming registers before either destination updates. The last stage's results leave the TEV whichever registers it wrote.
- A stage reading TEXC, TEXA or texture alpha with its lookup disabled (TEV order bit 6 clear; Resident Evil 4 draws these) samples nothing and reads 255 in every component, or 0 when BP genMode enables no texture coordinates. The constant ignores the swap table. This follows the behaviour Dolphin's `PixelShaderGen.cpp` and `UberShaderPixel.cpp` document; the no-coordinates case is marked unverified there.
- E0..E7 select separate regular and konst banks with BP bit 23; each bank keeps its own value.
- The fast model (`gx_fast_shader`) is visual fidelity: mediump float on normalized values, hardware filtering and LOD, GL blending. GX 8-bit rounding and dithering are not reproduced. Operands and output take their low 8 bits as GX does where a value can leave 0..255 (Super Mario Galaxy cuts Mario's eye holes with wrapped alphas).
- Bump paths are rejected in TEV capture; coordinate lookups belong to indirect capture.

## Indirect texturing

- Register layout and routing follow Petari `GXBump` and GX manual section 10. The offset model is a candidate, hardware unverified.
- Static two-row matrices decode signed-11 coefficients and a five-bit exponent. The third column's upper scale bit (bit 23), which the SDK still writes for exponents above 14, has no effect, matching Dolphin's register layout; Twilight Princess's heat haze relies on it.
- Offsets use S17.7 arithmetic: the matrix dot product truncates to seven fractional bits before the exponent, then the sum wraps to signed 24 bits.
- Formats: ABG samples in the 8-bit format with an optional -128 bias per component, or the low 5, 4 or 3 bits (GX_ITF_5/4/3) with an optional +1 bias (not -128).
- LOD bit (GXSetTevIndirect `utc_lod`, bit 19): the regular texture's LOD comes from the coordinate before wrapping and offset (Mario Party 4's tiled sprites).
- Wrap modes apply to the stage's regular S17.7 coordinate before the offset: modulo 256, 128, 64, 32 or 16 texels, or zero (GX_ITW_0; the depth-of-field blur uses it to look up a ramp by depth alone).
- Matrix-off stages without wrapping, feedback or bump alpha stay direct lookups whatever their unused selector, format, bias and LOD bits. Matrix-off stages that wrap take the S17.7 path with a zero offset.
- A stage selecting an indirect stage beyond the enabled count is undefined on the console; it adds no offset and keeps its wraps.
- One map may serve regular and indirect lookups in the same draw. Feedback, dynamic matrices and bump-alpha modes fail with an error.

## Texture sampling and formats

- Mipmapped images upload every captured level; cache identity and byte limits cover the whole chain including lower-level mutations. The shader computes an explicit floating candidate LOD from texel-space derivatives (longer edge, or longer diagonal when requested), then signed bias and min/max clamps. GLES supplies nearest-level or trilinear filtering. Anisotropy, bias clamp and differing min/mag spatial filters are rejected. Exact GX derivative and LOD quantization are unverified.
- CMPR needs an explicit arithmetic model because the manuals do not fix interpolation rounding or transparent RGB. Both candidates expand RGB565 endpoints with integer floor. `s3tc_candidate_v1` uses thirds and transparent black. `gx_candidate_v1` uses eighths and, for transparent selectors, the average endpoint RGB with alpha zero, so filtered cutout edges carry color. Default capture rejects CMPR until the caller selects a model. Neither is hardware-verified.
- Texture coordinates scale against the size the image register holds; a host extent keeps them normalized to the whole texture.
- Perspective draws use the homogeneous transform: W = -eyeZ and GX's [-W, 0] depth interval maps to GLES [-W, W]. GLES clips before the perspective divide and interpolates texture varyings with reciprocal W. Mali checks cover varying-W quads against screen-space barycentric predictions, near/far clipping and depth occlusion; this is not full game-image validation.

## Raster

- `capture_raster` owns the draw's viewport, clipped scissor, SU scales, pixel/depth/blend controls, field state and fog registers. The viewport is never clipped or rounded: clipping would change interpolation. Only the scissor intersects the EFB. Depth keeps its original GX units.
- GX inclusive scissor endpoints become a half-open lower-left rectangle.
- The quad offset (BP 0x59) moves pixels to EFB coordinates modulo 2048: a pixel e is drawn when `(e + offset) mod 2048` lies inside the scissor. `MR::drawFullScreenBlur` relies on this, drawing at y - 1584 to reach EFB row 464 below the 456-line frame. The default offset never reaches the wrapped interval; a scissor reaching both sides of the wrap fails.
- A scissor wholly in EFB rows 456..527 marks a strip draw, planned in that 72-row target's coordinates. A scissor crossing the frame boundary draws both parts as separate draws (GL state other than viewport and scissor depends on neither). GL draws nothing outside the viewport, so a draw reaches the strip only when its viewport does. Texture copies wholly inside the strip rows read the same target (unfiltered colour copies on the direct renderer only).
- Guard band: GX draws a primitive's pixels outside its viewport, up to the scissor; GL clips at the viewport. `GuardBand::reject` refuses a scissor reaching outside the viewport; `clip_to_viewport` clips and drops those pixels. GL viewports are whole pixels: `FractionalViewport::reject` refuses a fractional viewport, `round` moves each edge to the nearest pixel (at most half a pixel). The GPU sink must enforce `GL_MAX_VIEWPORT_DIMS` to prevent silent driver clamping.
- GX clamps each depth to 24 bits, so a range just past [0, 1] (Freezeflame's star select ends at 1.00001) clamps the same; GL clamps the whole range instead.
- GX front faces are clockwise on screen. The SDK swaps its public cull-mode bits into BP hardware encoding (back 1, front 2, both 3); the renderer maps that encoding to GLES face rejection with clockwise front faces. Culling disables the full-replacement shortcut, since a rejected primitive cannot replace existing EFB storage.
- RGB8 has no alpha plane. The SDK alpha-update flag is kept in the snapshot but has no effect in RGB8 and must not prevent an otherwise valid draw. Dithering also has no effect in RGB8 (GX manual 11.4); GLES dither stays disabled.
- Constant alpha while blending (`GXSetDstAlpha`): the destination alpha replaces only the stored alpha; colour blends with the TEV alpha (Freezeflame's ice).
- The exact domain rejects all fog. The fast domain admits it, below.

### Fog

BP 0xEE..0xF1 decode as the SDK's GXSetFog writes them: A and C as 20-bit floats (sign, exponent and top 11 mantissa bits of a binary32), B as a 24-bit magnitude over 2^24 with a shift, fsel and projection in 0xF1. The color is 0xF2. The range adjustment center is 0xE8 (minus 342) and its ten 8.8 factors are 0xE9..0xED.

The fast fragment program evaluates `x = A / (B - z * 2^-shift) - C`, or `A * z - C` for orthographic fog, with z the window depth. Depths behind the B plane are fully fogged, as the hardware divide saturates. With range adjustment the A term is scaled by the factor interpolated between samples; sample i lies (i+1)*32 pixels from the center column (GXInitFogAdjTable), the factor is 1 at the center, and the last sample lies beyond 320 pixels. x is clamped to [0, 1] and shaped by the linear, exp, exp2 or reverse curves, then mixes toward the fog color before GL blending; alpha is unchanged. fsel 0 is none, 2 linear, 4 exp, 5 exp2, 6 reverse exp, 7 reverse exp2; types 1 and 3 are rejected. With a Z texture, its Z replaces the window depth in the fog equation. Hardware unverified.

## Pixel arithmetic

`capture_pixel` owns BP00/41/42/43 and validates only the color operation, producing an immutable plan and uniforms. It does not admit a draw or interpret depth, fog, Z textures or alpha tests. `evaluate_pixel` takes RGBA8 source, canonical destination codes, EFB x/y and the caller's color-acceptance result. A rejected color or masked channel preserves the old code exactly.

- RGBA6 destinations hold four integers 0..63. RGB8 holds three bytes and an opaque alpha byte 255. A format change needs a separately specified conversion or clear; the module never reinterprets existing storage.
- Coordinates are within 640x456, x right, y down. In the lower-left GLES EFB, use `x = floor(gl_FragCoord.x)`, `y = 455 - floor(gl_FragCoord.y)`. Presentation and viewport-relative coordinates are wrong.
- RGB8 keeps the alpha-update bit in captured words but the effective plan disables alpha writes, preserving opaque backing alpha. Other unadmitted RGB8 controls fail.
- Admitted: RGB8/RGBA6 copy, blend and subtract; all eight factor codes; independent RGB/alpha masks; RGBA6 forced alpha; sixteen RGB8 logic functions. Rejected: logic/blend conflicts, RGBA6 logic width/order, RGB8 alpha-plane effects, RGB8 dither, RGB565/YUV, multisampling and reserved control bits. BP43 depth fields are retained but do not affect this color-only evaluator.
- Blend factors: the GXSetBlendMode tables in US 6,636,214 B1 separate source-slot destination-color factors from destination-slot source-color factors. Original source alpha stays the factor input when forced alpha replaces the final stored alpha.
- Six-bit dither arithmetic is retained from GX manual v1.03 pp124-127:

```
Q6 = (((c - (c >> 6)) << 2) + B[y & 3][x & 3]) >> 4
B = {{0,8,2,10}, {12,4,14,6}, {3,11,1,9}, {15,7,13,5}}
f9 = f + (f >> 7)
```

The manual does not settle the product tie convention, internal destination expansion, alpha dithering or reduction, or the nondithered reduction rule. `PixelPolicy::documented_only` (default) rejects operations needing them. `candidate_v1` selects, hardware unverified:

| Assumption bit | Candidate arithmetic |
| --- | --- |
| `product_ties_up` | Round each product as `(c*f9+128)>>8`, then add and saturate. |
| `destination_msb_expansion` | Expand a stored code as `(q<<2)\|(q>>4)` before blending/subtraction. |
| `alpha_truncation` | Reduce final or forced alpha with `a>>2`; RGB dither never changes alpha. |
| `nondither_rgb_truncation` | Reduce nondithered RGB with `c>>2`. |
| `subtract_saturation` | Apply destination-minus-source to each written component, including alpha, with a zero lower clamp. |

A captured snapshot reports every assumption it needs.

### Pixel GLSL

`pixel_glsl()` is a pure GLES 3.1 integer helper with no main, attachment, framebuffer fetch, depth operation or GL state:

```glsl
uvec4 gx_pixel(uvec4 source, uvec4 destination, ivec2 efb,
              bool color_accepted, uvec4 controls);
```

Both colors and coordinates follow the CPU valid-input contract. Upload `PixelSnapshot::uniforms()` as controls (BP41, BP42, format0/1, arithmetic version 1). The source is identical for all accepted plans; controls are per-draw uniforms. Include `pixel_glsl_version` in the containing program's key. Capture rejects unproven arithmetic unless requested before the shader is composed.

A fetch renderer may store RGBA6 codes in RGBA8 UNORM bytes as `q/255.0` and recover them before the helper. That needs matching clear, readback, copy and presentation behaviour: presenting codes directly looks dark. Fixed-function blending, GL dithering, sRGB conversion and MSAA must not alter an integer color-stage result.

`starlet_gx_pixel_tests --fixtures PREFIX` writes a compute shader and 4,880 little-endian test cases. A `.cases` record holds sixteen u32 values: source RGBA, destination RGBA, controls, then x/y/accepted/reserved. A `.expected` record holds four u32 output codes. The shader takes input/output SSBOs at bindings 0/1, `uCount`, and 64 invocations per group. Host tests also exhaust all 65,536 color/coefficient combinations, 1,048,576 logic cases, every dither input and phase, factor pairs, masks, rejection and forced-alpha ordering.

## Depth

`capture_depth` captures BP40/BP43, the Z texture (BP F4/F5) and the state revision for an ordinary non-antialiased linear Z24 pixel. It rejects reserved bits, compressed and 16-bit modes, multisampling and Z-freeze. An active Z texture makes the decision late.

- GX manual v1.03 (printed pp103, 122-123) distinguishes early and late Z. A passing early Z comparison may update depth even when alpha then rejects color. Late Z writes need both comparisons. Disabling Z comparison disables depth writes even with the update bit set.
- `evaluate_depth` takes two already-quantized unsigned 24-bit codes and returns color acceptance, a depth-write flag and the retained/new code. Both codes are checked even when comparison is disabled. The capture owns its state; later decoder changes cannot alter an earlier decision.
- `depth_glsl()` is the same bounded integer decision: `uvec2` controls (z-mode and pixel-control words, the latter with bit 6 clear under an active Z texture) and a `uvec3` result of color acceptance, depth write and retained/new depth. It assumes validated codes and controls and has no textures, uniforms, attachment operations or arithmetic-precision assumptions. Include `depth_glsl_version` in the containing shader key.
- The depth generator must independently justify converting GX viewport units to canonical codes.

### Z textures

`GXSetZTexture` writes the bias to BP F4 bits 0-23, the type (0 Z8, 1 Z16, 2 Z24X8) to BP F5 bits 0-1 and the op (0 disable, 1 add, 2 replace) to bits 2-3. An active op with a reserved op, type or bit fails; an inactive Z texture decodes to `{}`. The Z the depth test receives:

```
Z8:    z = A
Z16:   z = A << 8 | R
Z24X8: z = R << 16 | G << 8 | B
depth = (z + bias + (add ? reference : 0)) & 0xffffff
```

- The texel is the raw RGBA8 lookup of the last active TEV stage, before its swap table. Channels follow GX manual Appendix D: Z8 is an I8 texel, Z16 an IA8 texel with Z[15:8] in A, Z24X8 an RGBA8 texel with Z[23:0] in RGB. Section 8.9 notes copies to Z16 store their bytes reversed and are unusable as Z textures, which this mapping reproduces.
- The adders wrap without clamping. Add offsets the reference Z at the centre of the 2x2 quad without per-pixel slopes (section 8.10). EFB quads are window quads because target heights are even.
- The last active stage's texture input is the Z texel (section 9.12): `capture_tev` rejects a Z texture whose last stage has no lookup or reads its texture input. Z textures require the depth test after texturing (section 11.2), so an active Z texture ignores `GXSetZCompLoc`.
- Dolphin's software renderer, read for hardware behaviour only, applies the same mapping to the unswapped texel.
- Fragment programs implement this with `z_texture_glsl`: integer lanes of the quantized lookup, a `uGxZBias` uniform, the quad reference from `gl_FragCoord.z` and its derivatives, and `gl_FragDepth = depth / 2^24`. With the depth test enabled the program writes `gl_FragDepth` after the alpha test; GL compares and stores it with the GX function and update mask. Op, type and the depth-test enable are shader identity; the bias is the `uGxZBias` uniform (`TevUniforms::z_texture_bias`). Writing `gl_FragDepth` costs those draws Mali's early Z.
- JSystem's `JFWDisplay::clearEfb` draws this way: a C0-coloured screen quad whose 4x4 Z24X8 texture replaces Z with 0xffffff under an ALWAYS test with writes.

Mali validation covers all eight comparisons, depth-write masks, disabled comparison with update enabled, and alpha rejection before and after Z.

## EFB clear

`plan_efb_clear` describes a full-screen clear for a port that replaces a game's clear quad (a constant colour with an ALWAYS depth compare writing the far plane) with `EfbClearBackend::clear_efb`. It does not replace arbitrary GX draws and adds no depth-tested draw, copy, XFB or VI support.

- Admitted: original 640x456, non-AA, linear Z24, full viewport, no fog, noninterlaced. The plan keeps the RGB/alpha masks and intersects the quad with the current scissor.
- RGB8 ignores alpha writes and dithering (manual p127 limits color dithering to reduced-precision formats and enables it by default in GXInit). RGBA6 uses that manual's screen-aligned Bayer arithmetic with the `candidate_v1` alpha reduction and six-bit expansion from Pixel arithmetic above.
- EFB format reinterpretation is not implemented in the clear: format changes need a full replacement clear; partial or masked format changes are rejected before GL work.
- Depth clears to 0xffffff, which is GLES depth 1. The GLES backend uses one masked, scissored color/depth clear when the output is constant; a screen-aligned shader handles non-constant dither patterns after a depth clear.

## EFB format change

A change preserves the 24 physical color bits while reinterpreting three 8-bit or four 6-bit components. RGBA6 values use MSB expansion in the RGBA8 backing texture. A GPU pass applies the conversion before partial clears, draws or copies; a complete color replacement can omit it. Depth is preserved. The transfer surface is shared with display copy and VI and stays 640x456. Host tests exhaust all 24-bit format values.

## Texture copies

`plan_texture_copy` turns the BP texture-copy registers into a `TextureCopy`. `TextureCopyBackend` writes canonical tiled texture bytes into a 32-byte-aligned span the port validated, or keeps the copy GPU-resident under the span's token.

- Admitted: RGB8/RGBA6 EFB color or high-byte D24 depth, non-AA, progressive; unity size or 2x2 box reduction. Color outputs: RGB565, RGB5A3, RGBA8, A8, R8/G8/B8 and video-range I8 luminance. Other depth formats fail.
- The SDK selects the Z24 pixel format just for a depth copy, so a port decoding the GX stream may plan depth copies while PE control holds Z24; color copies under Z24 are refused.
- The copy uses its configured rectangle and tile-row stride, independently of the raster scissor. Source rectangles and clear masks keep their original EFB dimensions; only destination storage uses reduced dimensions.
- A rectangle reaching past the frame's right or bottom edge (or past the strip rows below it) is cut to them, as Dolphin does, and the destination keeps only the rows the cut rectangle covers. Pikmin 2's split-screen shadows (ShadowCylinder2 passes its bottom-right corner as the size) rely on it. What hardware reads past the EFB is unverified.
- Vertical filter: the seven taps fold into weights for the rows above, at and below each source row (64 is unity), as for display copies. Each RGB channel becomes the weighted sum shifted right by six, kept to nine bits and saturated to 255; alpha is the row's own. Outer rows clamp to the rectangle when its clamp bits are set and always to the EFB. Depth copies filter the three D24 bytes the same way. Rows below the frame take no filter: the blur's strips are written with it off, and screen-wobble loops copy one strip past the frame's last rows without reading it. Half-scale copies filter each source row before the 2x2 average; whether hardware rounds before or after that average is unknown.
- Filtered copies run the exact copy shader or, without GPU residency, read the neighbouring rows back.
- Copy-clear honors separate RGB, alpha and depth masks and the configured 24-bit Z.

### Copy byte layout

The GLES backend reads only the source rectangle, then encodes canonical tiled bytes:

| Tile | Formats | Bytes |
| --- | --- | --- |
| 4x4, two bytes per texel | RGB565, RGB5A3 (copy formats 3, 11, 12 and two-channel RA8/RG8/GB8) | 32 |
| 4x4 | RGBA8: AR plane then GB plane | 64 |
| 8x4, one byte per texel | A8, I8, R8, G8, B8 (copy formats 1, 7..10) | 32 |

- Two-channel formats store their first byte then their second: RA8 alpha then red, RG8 green then red, GB8 blue then green; they sample as IA8.
- A8 stores the expanded EFB alpha. I8 uses the display-copy luminance formula. RGB8 copies use opaque alpha.
- RGB5A3 writes opaque RGB5 when the truncated three-bit alpha is 7, otherwise A3RGB4. Cross-checked against Dolphin's software encoder; no code imported.
- Depth copies split D24 into high, middle and low bytes, take them as red, green and blue with opaque alpha, and encode like color: Z8 (format 1 or R8) stores the high byte, G8 and B8 the middle and low bytes, Z24X8 all three, and GX_TF_Z16, which the SDK copies as RG8, the middle byte then the high byte (so sampling it as IA8 gives the high byte as intensity). RGB565, RGB5A3 and A8 depth copies are rejected.
- Half-size copies average each 2x2 source block with integer truncation before packing. Tile-row padding and unused texels in partial edge tiles are untouched.
- Temporary CPU storage is bounded by one 640x456 RGBA8 rectangle (1,167,360 bytes). Host tests cover 132 format, depth, reduction and RGBA6 cases.
- With the direct renderer, copies stay GPU-resident. `decode_texture_copy` defines the texels the copy's sample format (I8, IA8, RGB565, RGB5A3 or RGBA8) decodes, and `store_texture_copy` writes exactly the encoder's bytes when a copy must be materialized. An I8 lookup taller than its copy can sample the copy over the memory rows after it (`decode_texture_copy_tail`), when the sampled format is I8 and every channel carries the one intensity (copy formats I8, R8, G8, B8).

Sources: pinned Petari `GXFrameBuf.c` and `GXTexture.c`, the GX manual's texture-copy and texture-format sections, and independent format arithmetic. Mali validation checks 72 RGB8/RGBA6 color, depth, channel, reduced-size and partial-tile cases, including padding, cropping, masked clears, format transitions and preservation of the source EFB.

## Display copy

`plan_display_copy` turns the BP display-copy registers into a `DisplayCopy`; `DisplayCopyBackend::copy_display` writes canonical XFB bytes into storage the port validated. An XFB becomes presentable only after a completed copy; a backend failure leaves it unpresented.

- Admitted: the original 640x456 EFB, RGB8/RGBA6, non-AA, progressive copy, gamma 1.0 and unity Y scale. Even source X and width, the copy rectangle, a 32-byte row stride and target capacity are checked. Other scale, gamma, field and format cases fail.
- The seven configured filter weights collapse to previous/current/next-row weights in the non-AA domain. The GPU applies them, converts to limited-range YUV and filters chroma horizontally at 1:2:1 before packing canonical Y0, U, Y1, V bytes. The decimal coefficients of JUTDirectPrint in the Petari decompilation define the integer conversion. An independent CPU reference checks the GPU arithmetic.
- A shared 640x456 RGBA8 GPU transfer texture (1,167,360 bytes) also stages EFB format reinterpretation; display copies use its left half. Readback writes the registered XFB with its row stride; padding stays untouched. No per-frame CPU pixel loop or temporary frame allocation.
- Copy-clear follows readback and honors independent RGB, alpha and depth-update masks, converts the clear color to the EFB format and preserves the requested 24-bit depth, independently of the raster scissor.
- Sources: pinned `GXFrameBuf.c`, `JUTDirectPrint.cpp`, GX manual section 12.5 and Advanced GX section 14. Muted hardware validation on an RG35XX SP (Mali-G31) compared RGB8/RGBA6 filtering, clear, padded subrectangles and selected older-XFB presentation against the reference.

## Peeks

`GXPeekZ` flushes the native command stream and synchronously reads the requested D24 EFB pixel. `GXPeekARGB` reads the stored RGBA8 colour directly. Coordinates convert from native top-left to GLES bottom-left. Draw-sync snapshots pack both at quarter resolution with fast shading: each row holds the packed depth, then the colour. Invalid destinations and missing backends fail without publishing a result.

## Renderer boundary

Facts about the GLES renderer that the GX model depends on. The renderer itself is described in [GLES-renderer.md](GLES-renderer.md).

- Capture validates owned vertex, transform, TEV, texture, raster and pixel snapshots at one state revision. The vertex cache shares immutable geometry across captures; hits compare current command and array bytes, including source encoding and implicit matrix selections. Per-draw revisions and state stay separate. Unavailable scoped array views bypass caching. Caller-owned snapshots can outlive eviction, so cache payload and queued-packet retention have separate budgets.
- Direct planning (`plan_direct_program`, `plan_direct_state`) derives the same program and per-draw state as `plan_gles_draw` from decoder registers instead of owned snapshots, so the direct renderer can cache programs by register identity. `direct_program_words` lists every register and layout-presence bit that can change the program: XF channel/texgen/projection controls, TEV stage, order, konst-selection and swap words, indirect commands/routing/scales, gen mode without cull, alpha-test functions without references, Z-texture, pixel format and Z-compare location, depth enable, RGBA6 blend/logic/subtract selection and the min filters of all eight maps. Equal words always plan the same program. Cull, scissor, viewport, blend factors, TEV registers, alpha references, the Z-texture bias, matrix values and texture images reach the GPU as raster state or uniforms. Host parity tests and an in-game on-device comparison of 72,112 draws found identical sources, raster plans, uniforms, transform rows and vertices.
- The direct path omits the CPU per-vertex transform proofs (clip-disabled domain, visible projective Q sign, overflow), the all-offscreen discard shortcut and full-screen replacement detection. The GPU evaluates the submitted values as given; an omitted full replacement only costs an EFB format reinterpretation.
- RGBA6 blending uses coherent ARM or EXT framebuffer fetch, discovered from the current context ([ARM](https://registry.khronos.org/OpenGL/extensions/ARM/ARM_shader_framebuffer_fetch.txt), [EXT](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_shader_framebuffer_fetch.txt)). A missing extension rejects destination-dependent draws before GL mutation. The shader fetches after TEV alpha comparison, converts expanded RGBA8 storage back to six-bit codes, and applies the integer pixel arithmetic. GL blending stays disabled. The fetch dialect is shader identity. Coherent fetch preserves API primitive order, including overlapping triangles inside one draw. Destination-dependent draws cannot be full EFB replacements. Mali-G31 validation checks all 64 factor pairs, subtract, write masks, forced alpha and dithering including overlapping quads; this shows agreement with the candidate arithmetic, not original-Wii precision. EXT generation has host coverage; the SP runtime exercises ARM fetch.
- Depth: comparison and update masks map to GLES state. Early Z uses a fragment-test qualifier, so alpha rejection keeps the configured depth-write ordering, which is shader identity. Depth-tested draws cannot be full EFB replacements.
- Programmable depth decision: fetch prior depth, evaluate the decision, and write unchanged destination color when early Z passes but alpha fails (discarding would lose the depth write). Hardware depth comparison must be ALWAYS; the depth mask is enabled only when the captured compare and update bits are both enabled. A rejected comparison retains both attachments. Texture derivative evaluation must stay well-defined. [ARM depth fetch](https://registry.khronos.org/OpenGL/extensions/ARM/ARM_shader_framebuffer_fetch_depth_stencil.txt) requires an attached depth image and disallows `early_fragment_tests`; its coherent ordering permits the decision without that qualifier.
- Depth encoding: an exhaustive probe round-tripped all 16,777,216 codes on the H700 Mali-G31 r20p0 using `float(code)/16777216.0` storage and `round(fetched*16777216.0)` recovery, with D24 and D32F attachments. Dividing by 16777215 failed on that driver. Other devices need the same capability and round-trip gate.
- The GLES depth attachment is a texture; `GXPeekZ` packs the sampled code with a one-pixel shader into a color readback, avoiding GLES depth-read format limits. Driver work runs in host allocation scope, including calls from the game's draw-sync callback.
- Texture upload keeps every supplied mip level and sampler field or explicitly rejects unsupported ones.
