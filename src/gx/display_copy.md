# Display copy

`plan_display_copy` turns the BP display-copy registers into a `DisplayCopy`, and
`DisplayCopyBackend::copy_display` writes canonical XFB bytes into storage the
port has validated. An XFB becomes presentable only after a completed copy; a
backend failure leaves it unpresented.

The admitted domain is the original 640x456 EFB, RGB8/RGBA6, non-AA, progressive
copy, gamma 1.0 and unity Y scale. Even source X/width, copy rectangle, 32-byte row
stride and target capacity are checked. Other scale/gamma/field/format cases fail
with an error.

The seven configured weights collapse to previous/current/next-row weights in
the non-AA domain. The GPU applies them, then converts to limited-range YUV and
filters chroma horizontally at 1:2:1 before packing canonical Y0,U,Y1,V bytes.
The decimal coefficients of JUTDirectPrint in the Petari decompilation define the
integer conversion. An independently written CPU reference checks the GPU arithmetic.
Primary format sources are pinned GXFrameBuf.c, JUTDirectPrint.cpp and
Nintendo's GX manual section 12.5 and Advanced Rendering section 14:
- https://pokeacer.xyz/wii/pdf/GX.pdf
- https://pokeacer.xyz/wii/pdf/Advanced_GX.pdf

A shared fixed 640x456 RGBA8 GPU transfer texture uses 1,167,360 bytes. It also
stages EFB format reinterpretation; display copies use its left half. Readback
writes the registered XFB with its row stride; padding remains untouched.
Conversion has no per-frame CPU pixel loop or temporary frame allocation.
Copy-clear follows readback and honors independent RGB, alpha and depth-update
masks. It converts clear color to EFB format and preserves the requested 24-bit
depth, independently of raster scissor.

Renderer boundary tests check inherited transfer state and failure latching. Muted
hardware validation on an RG35XX SP (Mali-G31) compared RGB8/RGBA6 filtering,
clear, padded subrectangles and selected older-XFB presentation against the
reference.
