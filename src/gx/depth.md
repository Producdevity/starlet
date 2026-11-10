# GX depth acceptance

This module evaluates the documented color/depth acceptance decision for an
ordinary non-antialiased linear Z24 pixel. It captures BP40/BP43, the Z texture
(BP F4/F5) and the state revision, rejecting reserved bits, compressed/16-bit
modes, multisampling and Z-freeze. An active Z texture makes the decision late.

The [GX manual v1.03](https://pokeacer.xyz/wii/pdf/GX.pdf), printed pp103 and
122–123, distinguishes early and late Z testing. A passing early Z comparison
may update depth even when alpha subsequently rejects color. Late Z writes
require both comparisons. Disabling Z comparison disables depth writes even
when the update bit remains set. Pinned Petari `GXPixel.c`, `GXEnum.h` and
`private/pe_reg.h` define the captured register fields and comparison codes.

`evaluate_depth` requires two already-quantized unsigned 24-bit codes. It returns
color acceptance, a depth-write flag and the resulting retained/new code. Both
codes are checked even when comparison is disabled. The immutable capture owns
its state; later decoder changes cannot alter an earlier decision.

`depth_glsl()` exposes the same bounded integer decision. Its `uvec2` controls
are the captured z-mode and pixel-control words, the latter with bit 6 clear under
an active Z texture; `uvec3` output lanes contain
color acceptance, depth write and retained/new depth. It assumes validated codes
and controls. There are no textures, uniforms, attachment operations, interpolation
or arithmetic precision assumptions in this helper. Include `depth_glsl_version`
in a containing shader key.

## Z textures

`GXSetZTexture` writes the bias to BP F4 bits 0-23 and the type (0 Z8, 1 Z16,
2 Z24X8) and op (0 disable, 1 add, 2 replace) to BP F5 bits 0-1 and 2-3.
`decode_z_texture` reads them; an active op with a reserved op, type or bit fails.
`z_texture_depth` is the Z the depth test receives:

```
Z8:    z = A
Z16:   z = A << 8 | R
Z24X8: z = R << 16 | G << 8 | B
depth = (z + bias + (add ? reference : 0)) & 0xffffff
```

The texel is the raw RGBA8 lookup of the last active TEV stage, before its swap
table. The channels follow the texel layouts of GX manual Appendix D: Z8 is an I8
texel, Z16 an IA8 texel with Z[15:8] in A, Z24X8 an RGBA8 texel with Z[23:0] in
RGB. Section 8.9 notes that copies to Z16 store their bytes reversed and are not
usable as Z textures, which this mapping reproduces. The adders wrap without
clamping, and add offsets the reference Z at the centre of the 2x2 quad without
the per-pixel slopes (section 8.10). The last active stage's texture
input is the Z texel (section 9.12): `capture_tev` rejects a Z texture whose last
stage has no lookup or reads its texture input. Z textures require the depth test
after texturing (section 11.2), so an active Z texture ignores `GXSetZCompLoc`.
Dolphin's software renderer, read for hardware behaviour only, applies the same
mapping to the unswapped texel.

The fragment programs implement this with `z_texture_glsl`: integer lanes of the
quantized lookup, a `uGxZBias` uniform, the quad reference from `gl_FragCoord.z`
and its derivatives (EFB quads are window quads, as target heights are even), and
`gl_FragDepth = depth / 2^24`, the encoding below. Only programs with an active
Z texture and an enabled depth test write `gl_FragDepth`, which costs those draws
Mali's early Z; fast programs with fog also compute the Z, which replaces the
window depth in the fog equation.

## Renderer boundary

The depth generator must independently justify the conversion from GX viewport
units to canonical codes.

A programmable implementation can fetch prior depth, evaluate this decision,
and write unchanged destination color when early Z passes but alpha fails.
Discarding that fragment would lose the required depth write. Hardware depth
comparison must be ALWAYS; the depth mask may be enabled only when the captured
compare and update bits are both enabled. A rejected comparison must retain
both attachments. Texture derivative evaluation must remain well-defined.

[ARM depth fetch](https://registry.khronos.org/OpenGL/extensions/ARM/ARM_shader_framebuffer_fetch_depth_stencil.txt)
requires an attached depth image and disallows `early_fragment_tests`. Its
coherent fetch ordering permits the programmable decision above without that
layout qualifier. This is separate from CPU arithmetic correctness.

The development probe exhaustively round-tripped all 16,777,216 codes on the
H700 Mali-G31 r20p0 using `float(code)/16777216.0` storage and
`round(fetched*16777216.0)` recovery, with both D24 and D32F attachments. It
also checked alpha/depth ordering and overlapping primitives. Dividing
by16777215 failed on that driver. These observations hold for that target's
depth storage. Other devices require the same capability/round-trip gate.
