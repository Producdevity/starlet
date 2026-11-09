# EFB clear

`plan_efb_clear` describes a full-screen clear in the current GX state, for a port
that replaces a game's clear quad (a constant colour with an ALWAYS depth compare
writing the far plane) with `EfbClearBackend::clear_efb`. It does not replace
arbitrary GX draws.

The plan retains the RGB/alpha masks and intersects the quad with the current
scissor. Only the original 640x456, non-AA, linear Z24, full viewport, no-fog,
noninterlaced domain is admitted. RGB8 ignores alpha writes and dithering; [GX
manual v1.03](https://pokeacer.xyz/wii/pdf/GX.pdf), printed p127, limits color
dithering to reduced-precision formats and enables it by default in GXInit. RGBA6
uses that manual's screen-aligned Bayer arithmetic. Alpha reduction and six-bit
expansion use the existing candidate_v1 convention documented in
[pixel.md](pixel.md). EFB format reinterpretation is not implemented: format
changes require a full replacement clear; partial or masked format changes are
rejected before GL work.

The GLES backend uses one masked, scissored color/depth clear when the output is
constant. A screen-aligned shader handles nonconstant dither patterns after a
depth clear. Both paths clear depth to 0xffffff, represented by GLES depth 1. This
adds no general depth-tested draw, copy, XFB or VI presentation support.
