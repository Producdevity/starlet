#pragma once

#include <dolphin/gx/GXAurora.h>

#include <cstddef>
#include <cstdint>

// The Aurora subcommands the bridge decodes: ids, and the payload bytes after the 3-byte header
// (GX_AURORA, u16 id), as lib/gx/command_processor.cpp and the writers in lib/dolphin/gx read and
// write them. Ids are checked against GXAurora.h at compile time. An id that Aurora renumbers
// fails the build here; a payload that changes size must be updated by hand in this table.
//
// Aurora trees differ in which commands they have, and share ids with different meanings:
//   fork     bmdhacks/aurora 3fbacee (GLES backend): 0x38 is REQUEST_DEPTH_SNAPSHOT, no payload.
//   upstream encounter/aurora, which grows over time: 5179721 has none of 0x38, 0x3b and 0x52;
//            5794daa has 0x38 as SET_TEX_IMG_PTR (u8 map, u64 pointer), 0x3b and 0x52.
// Each optional command is compiled in when GXAurora.h defines its macro, so the table follows
// the tree the bridge is built against. STARLET_AURORA_UPSTREAM selects the host glue (the fork
// owns its GL context through its render worker; upstream trees do not).
#ifdef GX_AURORA_REQUEST_DEPTH_SNAPSHOT
#define STARLET_AURORA_UPSTREAM 0
#else
#define STARLET_AURORA_UPSTREAM 1
#endif

namespace aurora::starlet_backend::commands {

constexpr bool upstream = STARLET_AURORA_UPSTREAM != 0;

// Commands with a fixed payload; both generations.
struct Fixed {
  std::uint16_t id;
  std::uint8_t payload;
};

constexpr std::uint16_t viewport_render = 0x0001;       // 6 x f32
constexpr std::uint16_t scissor_render = 0x0002;        // 4 x u32
constexpr std::uint16_t projection_full = 0x0003;       // 16 x f32
constexpr std::uint16_t arraybase = 0x0010;             // + slot 0..15: u64 pointer, u32 size, u8 little-endian
constexpr std::uint16_t debug_group_push = 0x0020;      // u16 length, characters
constexpr std::uint16_t debug_group_pop = 0x0021;
constexpr std::uint16_t debug_marker_insert = 0x0022;   // u16 length, characters
constexpr std::uint16_t load_texobj = 0x0030;           // u8 map, u64 data, u32 w, h, format, tlut, u8 mips, u32 object id, u32 version
constexpr std::uint16_t load_tlut = 0x0031;             // u8 name, u64 data, u32 format, u16 entries, u32 object id, u32 version
constexpr std::uint16_t destroy_texobj = 0x0032;        // u32 object id
constexpr std::uint16_t destroy_tlut = 0x0033;          // u32 object id
constexpr std::uint16_t destroy_copy_tex = 0x0034;      // u64 destination
constexpr std::uint16_t load_copy_src = 0x0035;         // 4 x u32
constexpr std::uint16_t load_copy_dst = 0x0036;         // u32 w, h, format, u8 mipmap
constexpr std::uint16_t load_copy_dest = 0x0037;        // u64 destination
constexpr std::uint16_t begin_offscreen = 0x0039;       // u32 w, h
constexpr std::uint16_t end_offscreen = 0x003a;
constexpr std::uint16_t draw_sized = 0x0040;            // u8 opcode, u32 bytes, vertex data
constexpr std::uint16_t draw_indexed = 0x0041;          // u8 opcode, u16 vertices, u32 indices, indices as u16, vertex data
constexpr std::uint16_t polygon_offset = 0x1000;        // 5 x f32

static_assert(viewport_render == GX_AURORA_LOAD_VIEWPORT_RENDER);
static_assert(scissor_render == GX_AURORA_LOAD_SCISSOR_RENDER);
static_assert(projection_full == GX_AURORA_LOAD_PROJECTION_FULL);
static_assert(arraybase == GX_AURORA_LOAD_ARRAYBASE);
static_assert(debug_group_push == GX_AURORA_DEBUG_GROUP_PUSH);
static_assert(debug_group_pop == GX_AURORA_DEBUG_GROUP_POP);
static_assert(debug_marker_insert == GX_AURORA_DEBUG_MARKER_INSERT);
static_assert(load_texobj == GX_AURORA_LOAD_TEXOBJ);
static_assert(load_tlut == GX_AURORA_LOAD_TLUT);
static_assert(destroy_texobj == GX_AURORA_DESTROY_TEXOBJ);
static_assert(destroy_tlut == GX_AURORA_DESTROY_TLUT);
static_assert(destroy_copy_tex == GX_AURORA_DESTROY_COPY_TEX);
static_assert(load_copy_src == GX_AURORA_LOAD_COPY_SRC);
static_assert(load_copy_dst == GX_AURORA_LOAD_COPY_DST);
static_assert(load_copy_dest == GX_AURORA_LOAD_COPY_DEST);
static_assert(begin_offscreen == GX_AURORA_BEGIN_OFFSCREEN);
static_assert(end_offscreen == GX_AURORA_END_OFFSCREEN);
static_assert(draw_sized == GX_AURORA_DRAW_SIZED);
static_assert(draw_indexed == GX_AURORA_DRAW_INDEXED);
static_assert(polygon_offset == GX2_SET_POLYGON_OFFSET);

#ifdef GX_AURORA_REQUEST_DEPTH_SNAPSHOT
constexpr std::uint16_t request_depth_snapshot = 0x0038; // no payload
static_assert(request_depth_snapshot == GX_AURORA_REQUEST_DEPTH_SNAPSHOT);
#endif
#ifdef GX_AURORA_SET_TEX_IMG_PTR
constexpr std::uint16_t set_tex_img_ptr = 0x0038;       // u8 map, u64 data
static_assert(set_tex_img_ptr == GX_AURORA_SET_TEX_IMG_PTR);
#endif
#ifdef GX_AURORA_LOAD_DISP_COPY_SRC
constexpr std::uint16_t load_disp_copy_src = 0x003b;    // 4 x u32
static_assert(load_disp_copy_src == GX_AURORA_LOAD_DISP_COPY_SRC);
#endif
#ifdef GX_AURORA_SET_DRAWS_BLOCKING
constexpr std::uint16_t set_draws_blocking = 0x0042;    // u8
static_assert(set_draws_blocking == GX_AURORA_SET_DRAWS_BLOCKING);
#endif
#ifdef GX_AURORA_READBACK_CAPTURE_PEEKS
constexpr std::uint16_t readback_peeks = 0x0050;        // u64 stream, u64 sequence, u32 count, u8 alpha mode, count x 13
static_assert(readback_peeks == GX_AURORA_READBACK_CAPTURE_PEEKS);
#endif
#ifdef GX_AURORA_READBACK_CAPTURE_COPIES
constexpr std::uint16_t readback_copies = 0x0051;       // u64 stream, u64 sequence, u32 count, count x 16
static_assert(readback_copies == GX_AURORA_READBACK_CAPTURE_COPIES);
#endif
#ifdef GX_AURORA_CLEAR
constexpr std::uint16_t clear = 0x0052;                 // u8 flags, 4 x u32 rectangle, 4 x u8 colour, u32 depth
static_assert(clear == GX_AURORA_CLEAR);
#endif

// Payload sizes of the fixed commands; the first ones are the same in both generations.
constexpr Fixed fixed[] = {
    {viewport_render, 24},
    {scissor_render, 16},
    {projection_full, 64},
    {load_texobj, 34},
    {load_tlut, 23},
    {destroy_texobj, 4},
    {destroy_tlut, 4},
    {destroy_copy_tex, 8},
    {load_copy_src, 16},
    {load_copy_dst, 13},
    {load_copy_dest, 8},
    {begin_offscreen, 8},
    {end_offscreen, 0},
    {debug_group_pop, 0},
    {polygon_offset, 20},
#ifdef GX_AURORA_REQUEST_DEPTH_SNAPSHOT
    {request_depth_snapshot, 0},
#endif
#ifdef GX_AURORA_SET_TEX_IMG_PTR
    {set_tex_img_ptr, 9},
#endif
#ifdef GX_AURORA_LOAD_DISP_COPY_SRC
    {load_disp_copy_src, 16},
#endif
#ifdef GX_AURORA_SET_DRAWS_BLOCKING
    {set_draws_blocking, 1},
#endif
#ifdef GX_AURORA_CLEAR
    {clear, 25},
#endif
};

// Fixed-size part of the variable-length commands.
constexpr std::size_t arraybase_payload = 13;
constexpr std::size_t draw_sized_header = 5;
constexpr std::size_t draw_indexed_header = 7;
constexpr std::size_t readback_header = 20;
constexpr std::size_t readback_peek_entry = 13, readback_copy_entry = 16;

constexpr bool find_fixed(std::uint16_t id, std::size_t& payload) {
  for (const auto& entry : fixed) {
    if (entry.id == id) {
      payload = entry.payload;
      return true;
    }
  }
  return false;
}

// The table has no id twice.
constexpr bool unique_ids() {
  for (std::size_t i = 0; i < sizeof fixed / sizeof fixed[0]; ++i) {
    for (std::size_t j = i + 1; j < sizeof fixed / sizeof fixed[0]; ++j) {
      if (fixed[i].id == fixed[j].id) {
        return false;
      }
    }
  }
  return true;
}
static_assert(unique_ids());

} // namespace aurora::starlet_backend::commands
