# Direct GX draw planning

`direct.hpp` derives the GLES program and per-draw state of `plan_gles_draw`
from decoder registers instead of owned draw snapshots. It exists so the
direct renderer can cache programs by register identity and skip the snapshot
and per-vertex proof work of the packet path.

`direct_program_words` lists every register and layout-presence bit that can
change the program: XF channel/texgen/projection controls, TEV stage, order,
konst-selection and swap words, indirect commands/routing/scales, gen mode
without cull, alpha-test functions without references, Z-texture, pixel format
and Z-compare location, depth enable, RGBA6 blend/logic/subtract selection and
the min filters of all eight maps. Equal words always plan the same program.
Cull, scissor, viewport, blend factors, TEV registers, alpha references, the
Z-texture bias, matrix values and texture images reach the GPU as raster state or
uniforms.

`plan_direct_program` runs the existing `capture_tev`, `capture_indirect`,
`capture_transform_pipeline` and `capture_pixel` rejection rules and emits the
same vertex and fragment GLSL as `plan_gles_draw` through the shared
`gles_fragment_source`. `plan_direct_state` runs `capture_raster`,
`plan_gles_raster`, `describe_texture` and the sampling checks, producing the
uniform values `plan_gles_draw` computes. Host parity tests and an in-game
on-device comparison of 72,112 draws found identical sources, raster plans,
uniforms, transform rows and vertices.

Omitted: the CPU per-vertex transform proofs (clip-disabled domain,
visible projective Q sign, overflow), the all-offscreen discard shortcut and the
full-screen replacement detection. The GPU evaluates the submitted values as
given; an omitted full replacement only costs an EFB format reinterpretation.
