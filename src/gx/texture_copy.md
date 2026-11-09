# EFB texture copies

`plan_texture_copy` turns the BP texture-copy registers into a `TextureCopy`.
`TextureCopyBackend` writes canonical tiled texture bytes into a 32-byte aligned
span the port has validated, or keeps the copy GPU-resident under the span's
token.

The supported domain is RGB8/RGBA6 EFB color or high-byte D24 depth, non-AA and
progressive. Copies may use unity size or the requested 2x2 box reduction. Color
outputs include RGB565, RGB5A3, RGBA8, A8, R8/G8/B8 and video-range I8 luminance.
Other depth formats fail with an error. The copy uses its configured rectangle and
tile-row stride, independently of raster scissor.

The vertical filter's seven taps fold into weights for the rows above, at and
below each source row, as for display copies. Each RGB channel becomes the
weighted sum shifted right by six, kept to nine bits and saturated to 255; alpha
is the row's own. Outer rows clamp to the rectangle when its clamp bits are set
and always to the EFB. Depth copies filter the three D24 bytes the same way.
Half-scale copies filter each source row before the 2x2 average; whether the
hardware rounds before or after that average is unknown. Filtered copies run the
exact copy shader or, without GPU residency, read the neighbouring rows back. Rows
below the frame take no filter. Copy-clear honors separate RGB, alpha and depth
masks and the configured 24-bit Z.

The GLES backend reads only the source rectangle, then encodes canonical tiled
bytes: 4x4 RGB565/RGB5A3 tiles (32 bytes), 4x4 RGBA8 tiles (64 bytes, AR/GB
planes), 4x4 two-channel tiles (32 bytes), or 8x4 single-channel tiles (32
bytes). The two-channel formats store their first byte then their second: RA8
alpha then red, RG8 green then red, GB8 blue then green; they sample as IA8. A8
stores the expanded EFB alpha, and I8 uses the display-copy luminance formula.
Depth copies split D24 into its high, middle and low bytes, take them as red,
green and blue with opaque alpha, and encode them like color: Z8 (format 1 or R8)
stores the high byte, G8 and B8 the middle and low bytes, Z24X8 all three, and
GX_TF_Z16, which the SDK copies as RG8, the middle byte then the high byte.
RGB565, RGB5A3 and A8 depth copies are rejected.
RGB8 copies use opaque alpha. RGB5A3 selects opaque RGB5 when the truncated
three-bit alpha is 7; otherwise it writes A3RGB4. Tile-row padding and unused
texels in partial edge tiles are untouched. Half-size copies average each 2x2
source block with integer truncation before packing. Source rectangles and clear
masks retain their original EFB dimensions; only destination storage uses the
reduced dimensions. This does not lower the game EFB resolution or change which
effects run. This is the synchronous CPU path. With the direct renderer, copies
instead stay GPU-resident (direct.md): `decode_texture_copy` defines the texels
the copy's sample format (I8, IA8, RGB565, RGB5A3 or RGBA8) decodes, and
`store_texture_copy` writes them back as exactly the encoder's bytes when a copy
must be materialized. Host tests cover 132 format, depth, reduction and RGBA6 cases.
Temporary CPU storage is bounded by one 640x456 RGBA8 rectangle (1,167,360 bytes).

An EFB format change preserves the physical 24 color bits while reinterpreting
three 8-bit or four 6-bit components. RGBA6 values use MSB expansion in the RGBA8
backing texture. A GPU pass applies that conversion before partial clears, draws
or copies; complete color replacement can omit it. It preserves depth. The
transfer surface is shared with display copy/VI and remains 640x456.

The contracts come from pinned Petari GXFrameBuf.c/GXTexture.c, the GX manual's
texture-copy and texture-format sections, and independent format arithmetic. The
RGB5A3 alpha branch was cross-checked against Dolphin's software encoder; no
implementation was imported. Host tests exhaust all 24-bit format values, check
and fixed texture bytes. Existing Mali validation checks pixels, cropping, tile
padding, masked clears and format transitions. Dedicated Mali checks cover 72
RGB8/RGBA6 color, depth, channel, reduced-size and partial-tile cases, including
padding and preservation of the source EFB.
