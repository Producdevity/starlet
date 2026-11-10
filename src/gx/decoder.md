# GX command core

`starlet_gx` is a C++17 library with no GL, SDL or game dependency; CTest
registers its decoder tests as `starlet_gx_core`. The renderer implements
`CommandSink` and the port provides `MemoryReader` over registered GX address
tokens. No FIFO address is cast to a native pointer. The memory reader returns
canonical big-endian GX bytes. Native mutable arrays/matrices need typed views
or conversion; copying host little-endian memory into this interface is
incorrect.

Use `Decoder::feed` for big-endian FIFO/display-list bytes, or the typed
`write_u8/u16/u32/f32` functions for a port's write-gather replacements. All
paths mutate the same state in command order. `finish` checks a submission
boundary for a truncated command; it does not clear state. Errors latch, and
commands already completed remain applied. Discard a failed decoder. Exceptions
from callbacks or decoder allocation also latch failure before propagating with
their original type. The exception failure message is empty so handling an
allocation failure does not itself allocate. After catching one, subsequent
feeds cannot replay a partially processed command.

The decoder buffers at most one bounded command, reads every display-list call
into a fresh owned snapshot, and limits nesting, expanded input and command
count per feed. Nesting has an absolute ceiling of 64. These are configurable
work limits. Display-list contents must remain stable during
`MemoryReader::copy`.

A draw with per-vertex matrix indices leaves its last enabled vertex's indices as
the current matrix indices, without changing the XF register image. Every write
of XF 0x1018/0x1019 restores the indices from the written word, including a write
of the value already held: J3D reloads an unchanged current matrix after skinned
shapes, and skipping it draws the next shape with a skinned joint's matrix.

Draw callbacks are synchronous. The state and raw vertex bytes are valid during
the callback; indexed vertex arrays and textures must be read/snapshotted there
before returning. Attribute `components` counts all components per vertex (nine
for NBT); `indices` distinguishes one NBT index from three separate indices.
The renderer still owns vertex expansion, reserved position-index skip markers,
primitive assembly, conversion/quantization and all rendering semantics.
A future asynchronous renderer must own all referenced data,
retain immutable palette blocks, and separately retire CPU and GPU resources.
No packet queue, GL resource lifetime or rendering implementation is supplied.
Every applied BP write reaches the sink, which must reject operations it cannot
implement. CP state, XF writes, four indexed XF arrays, VCD/VAT vertex sizing,
TLUT loads and vertex-cache invalidation are decoded here. Unsupported opcodes,
CP addresses, formats and out-of-range transfers fail with an error.

Palette offsets are relative to TMEM `0x80000`. A load creates immutable blocks
and advances generations even if the address and BP command repeat within the
same frame. A texture cache key must include its palette range generation and
format. Generations conservatively cover 512-byte blocks; no cache may treat an
unloaded palette range as valid. State copies preserve prior palette contents. Generations are local to one
Decoder lifetime. When replacing a decoder, a renderer retaining texture caches
must clear those caches or include its own decoder epoch in every palette cache
key; a new decoder may reuse the same numeric generations.

Indexed XF loads (CP opcodes 0x20, 0x28, 0x30 and 0x38) read through
`MemoryReader::copy_indexed_xf`, which names the array (12 to 15) and defaults to
`copy`. Games compute those matrices and lights on the CPU every frame, so a port
whose CPU writes host-order numbers converts them there without describing every
matrix buffer; the words it returns are canonical like any other read.

## Frame layout

`Decoder` takes the `EfbLayout` its state reports: the frame a game's render mode
draws, 640 wide and `height` rows of the 528-row EFB. Planners for rasterization,
copies and clears bound their rectangles by it. Give every decoder and renderer of
one port the same layout.

## Merged primitives

With `Limits::merge_primitives`, back-to-back quad, triangle, strip and fan commands
of one VAT reach the sink as one `Draw`. No command separates them, so one state
draws them all. `Draw::runs` lists each primitive's opcode, vertex count and the
offset of its first vertex in `vertices`, which spans every run including the
three-byte header before each later one. `primitive` and `vertex_count` describe
the first run; `total_vertices()` counts all. Merging stops at any other command, a
VAT change, a line or point command, a primitive without vertices, the end of the
fed bytes and 65535 vertices. The last enabled vertex of the last run latches the
matrix indices.

Display lists strip their meshes into many short primitives. Without merging, a
sink pays its per-draw work for each one: Twilight Princess's field draws about
10000 primitives a frame, and merging turns them into about 200 draws. Enable it
only for sinks that read `runs`; `GlesRenderer::draw_direct` does, and
`capture_vertices` and `GlesRenderer::draw` reject merged draws.

## Host texture extents

GX image registers hold sizes up to 1024 texels. A PC port's own textures can be
larger: Twilight Princess's PC build joins a font's pages into one 128x1152 image.
`Decoder::set_texture_extent(map, {width, height})` gives a map that size while its
image register holds the extent's low ten bits in each dimension; any other register
value ignores the extent. Texture coordinates keep scaling against the register
size, so they stay normalized to the whole texture. An extent change counts as a
BP change.

## Fast commands

NOP runs, plain BP writes (both TEV banks), plain CP writes and XF matrix-memory writes of
up to 16 words are most commands. `fast_command` applies them directly, with the general
path's state, revisions and `bp_write` callbacks, at the stream's top level and inside
display lists; games such as Pikmin 2 send most of their material and shape state through
lists (about 590 KB of list bytes per title frame). A list's bytes are charged when the
list is called, so commands inside it charge only their count. Anything else (TLUT loads,
texture-cache invalidation, masked writes, XF registers, indexed loads, primitives, host
opcodes) or a limit takes the general path. `Decoder::counts()` reports how many commands
each path took, by opcode for the general path.

## Host commands

A PC port's FIFO can carry the port's own commands: Aurora's opcode 0x50 names host
pointers and stands for the register writes a console made. Set their opcodes in
`Limits::host_opcodes`; `CommandSink::host_command` then receives each one at its first
byte with the rest of the fed bytes, sets `consumed` to its size and may apply the
commands it stands for with `Decoder::inject`. A host command must arrive whole in one
feed: give the decoder a drained FIFO buffer in one `feed`, starting at a command
boundary.

`Limits::filtered_bp` names BP addresses whose stream writes pass through
`CommandSink::filter_bp` before they apply. The sink may rewrite the value, drop the
write, or run the operation itself and inject the writes it stands for. `Limits::dropped_bp`
names addresses whose stream writes are ignored without a callback, for registers the host
always supplies itself.

Injected commands are hardware commands: they skip `host_opcodes` and `filter_bp`. A
display list an injected `CALL_DL` runs decodes as stream commands, so a host can turn
its own list call into a real one.

`inject` applies whole commands at the current stream position, under the running
feed's work limits. It works between feeds and from `host_command` and `filter_bp`;
from any other callback, or while a partial command is buffered, it fails with
`reentrant_call`.

## Sources

Independent implementation against the GX register definitions in the Petari
decompilation's SDK (`private/cp_reg.h`, `private/xf_mem.h`, `private/tx_reg.h`)
and its GX command emitters (`GXAttr.c`, `GXTexture.c`, `GXDisplayList.c`,
`GXTransform.c`, `GXVert.h`, `GXRegs.h`). The [Nintendo GX manual
v1.00](https://www.davidgf.net/downloads/gcwii/gx.pdf) sections 4.3–4.6, 7 and
Appendix C describe vertex/display-list state semantics. Tests call this
implementation with independently specified register values, byte streams and
expected snapshots. No predecessor port implementation or shader/test source is
used.