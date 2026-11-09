# Stream vertex loading

`append_stream_vertices` converts one GX draw into an interleaved record holding
only the present attributes (`StreamFormat`), in the transform shader's input
units, plus triangle-list indices. Values are bit-identical to `capture_vertices`
for every position, normal/NBT, color, texture-coordinate and matrix-index
format, canonical or native-endian array, view or copy source; a randomized host
test compares 4,000 layouts. Disabled vertices keep zeroed slots, fully
disabled primitives are dropped, and partially disabled primitives, lines and
points are rejected. Failures restore both output vectors. `append_stream_records`
converts the vertices of a line or point draw the same way, without indices, and
rejects index-disabled vertices; the caller expands them (`gx_line_point`).

Layouts without explicit matrix indices carry no per-vertex matrix words; the
renderer supplies `stream_matrix_latches` as a constant attribute. Optional
`StreamArraySpans` report the consumed byte range of each indexed array so a
caller can validate reuse of converted geometry.

A merged draw (`Draw::runs`) converts as one: its runs' vertices are gathered into a
contiguous buffer, converted once, and each run's primitive emits its own indices
against its offset in the records. The result equals converting the runs one by one
and offsetting their indices. Array spans then cover every run. Use
`append_stream_vertices` for merged draws; `append_stream_records` rejects them.

Host-order arrays (`MemoryArrayView::host_order`) hold numbers in host byte order at
their own width, as a PC port's native code writes them; they convert like native
views of that width.
