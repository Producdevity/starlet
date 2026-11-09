# GX depth acceptance

This module evaluates the documented color/depth acceptance decision for an
ordinary non-antialiased linear Z24 pixel. It captures BP40/BP43 and the state
revision, rejecting reserved bits, compressed/16-bit modes, multisampling,
Z-freeze and active Z textures. It does not enable those draws in the renderer.

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
are the captured z-mode and pixel-control words; `uvec3` output lanes contain
color acceptance, depth write and retained/new depth. It assumes validated codes
and controls. There are no textures, uniforms, attachment operations, interpolation
or arithmetic precision assumptions in this helper. Include `depth_glsl_version`
in a containing shader key.

## Renderer boundary

The depth generator must independently justify the conversion from GX viewport
units to canonical codes. Z-texture generation is a separate requirement,
including the game's textured EFB clear path.

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
