#include "aurora_stream.hpp"
#include "aurora_commands.hpp"
#include "issues.hpp"
#include "texture_convert.hpp"

#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXCommandList.h>
#include <dolphin/gx/GXEnum.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fmt/format.h>
#include <utility>

extern "C" u32 GXGetTexBufferSize(u16 width, u16 height, u32 fmt, GXBool mips, u8 maxLod);

namespace aurora::starlet_backend {
namespace {
std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }
std::uint32_t be32(const std::uint8_t* p) {
  return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 | p[3];
}
std::uint64_t be64(const std::uint8_t* p) { return std::uint64_t{be32(p)} << 32 | be32(p + 4); }
void put32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 24);
  p[1] = static_cast<std::uint8_t>(v >> 16);
  p[2] = static_cast<std::uint8_t>(v >> 8);
  p[3] = static_cast<std::uint8_t>(v);
}
const void* host_pointer(std::uint64_t value) { return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(value)); }

// TMEM line (512 bytes) where Aurora's TLUT name keeps its palette: GX_TLUT0..15 hold up to
// 256 entries each, GX_BIGTLUT0..3 up to 16384.
std::uint32_t tlut_line(std::uint32_t name) { return name < 16 ? name : 64 + (name - 16) * 64; }

std::uint8_t texture_register(std::uint8_t base_low, std::uint8_t base_high, std::uint8_t map) {
  return map < 4 ? static_cast<std::uint8_t>(base_low + map) : static_cast<std::uint8_t>(base_high + map - 4);
}

// Width of a copy format's tile in texels, and 32-byte blocks per tile (GXSetTexCopyDst).
void copy_tile(std::uint32_t format, std::uint32_t& width, std::uint32_t& blocks) {
  blocks = 1;
  switch (format) {
  case GX_TF_I4: case GX_TF_C4: case GX_CTF_R4: case GX_CTF_Z4:
    width = 8; break;
  case GX_TF_I8: case GX_TF_IA4: case GX_TF_C8: case GX_TF_Z8: case GX_CTF_RA4: case GX_CTF_A8: case GX_CTF_R8:
  case GX_CTF_G8: case GX_CTF_B8: case GX_CTF_Z8M: case GX_CTF_Z8L:
    width = 8; break;
  case GX_TF_RGBA8: case GX_TF_Z24X8: case GX_CTF_YUVA8:
    width = 4; blocks = 2; break;
  default:
    width = 4; break;
  }
}
} // namespace

AuroraStream::AuroraStream(HostMemory& memory, starlet::gx::Decoder& decoder, EfbTarget& efb)
: memory_(memory), decoder_(decoder), efb_(efb) {}

void AuroraStream::fail(std::string message) {
  if (failure_.empty()) {
    failure_ = std::move(message);
  }
}

void AuroraStream::write(const std::uint8_t* bytes, std::size_t size) {
  const auto status = decoder_.inject({bytes, size});
  if (!status) {
    fail("decoder: " + status.message);
  }
}

void AuroraStream::write_cp(std::uint8_t address, std::uint32_t value) {
  std::uint8_t bytes[6] = {0x08, address};
  put32(bytes + 2, value);
  write(bytes, sizeof bytes);
}

void AuroraStream::write_bp(std::uint32_t word) {
  std::uint8_t bytes[5] = {0x61};
  put32(bytes + 1, word);
  write(bytes, sizeof bytes);
}

void AuroraStream::write_xf(std::uint16_t address, const std::uint32_t* values, std::size_t count) {
  std::array<std::uint8_t, 5 + 16 * 4> bytes{0x10};
  if (count == 0 || count > 16) {
    fail("XF register write of " + std::to_string(count) + " words");
    return;
  }
  put32(bytes.data() + 1, static_cast<std::uint32_t>(count - 1) << 16 | address);
  for (std::size_t i = 0; i < count; ++i) {
    put32(bytes.data() + 5 + i * 4, values[i]);
  }
  write(bytes.data(), 5 + count * 4);
}

// GXCreateFrameBuffer stands in for the GameCube code's GXSetViewport(0, 0, w, h, 0, 1) and
// GXSetScissor(0, 0, w, h); GXRestoreFrameBuffer puts back the frame's viewport and scissor, as
// Aurora's own renderer does with its render-pass state.
void AuroraStream::begin_offscreen(std::uint32_t width, std::uint32_t height) {
  const auto& state = decoder_.state();
  if (!suspended_view_) {
    SuspendedView view;
    for (std::uint16_t i = 0; i < 6; ++i) {
      view.viewport[i] = state.xf(0x101a + i);
    }
    view.scissor0 = 0x20000000u | (state.bp(0x20) & 0xffffff);
    view.scissor1 = 0x21000000u | (state.bp(0x21) & 0xffffff);
    suspended_view_ = view;
  }
  const float w = static_cast<float>(width), h = static_cast<float>(height);
  const float viewport[6] = {w / 2.0f, -h / 2.0f, 16777215.0f, 342.0f + w / 2.0f, 342.0f + h / 2.0f, 16777215.0f};
  std::uint32_t words[6];
  std::memcpy(words, viewport, sizeof words);
  write_xf(0x101a, words, 6);
  write_bp(0x20000000u | 342u << 12 | 342u);
  write_bp(0x21000000u | (342u + width - 1) << 12 | (342u + height - 1));
  efb_.begin_offscreen();
}

void AuroraStream::end_offscreen() {
  efb_.end_offscreen();
  if (suspended_view_) {
    write_xf(0x101a, suspended_view_->viewport.data(), 6);
    write_bp(suspended_view_->scissor0);
    write_bp(suspended_view_->scissor1);
    suspended_view_.reset();
  }
}

bool AuroraStream::vertex_size(std::uint8_t vat, std::size_t& size) {
  auto& cached = strides_[vat & 7];
  const auto revision = decoder_.state().cp_layout_revision() + 1;
  if (cached.first == revision) {
    size = cached.second;
    return true;
  }
  starlet::gx::VertexLayout layout;
  std::string error;
  if (!decoder_.state().vertex_layout(vat, layout, error)) {
    fail("vertex layout: " + error);
    return false;
  }
  size = layout.stride;
  cached = {revision, size};
  return true;
}

void AuroraStream::configure(starlet::gx::Limits& limits) {
  limits.host_opcodes.set(0x40);
  limits.host_opcodes.set(GX_AURORA);
  // Indexed XF loads resolve the matrix array they read first (see matrix_arrays_).
  for (unsigned op : {0x20u, 0x28u, 0x30u, 0x38u}) {
    limits.host_opcodes.set(op);
  }
  limits.filtered_bp.set(0x52);
  if (commands::upstream) {
    limits.filtered_bp.set(0x59);
  }
  // TLUT loads replay with the palette's address when LOAD_TLUT arrives. Image and TLUT
  // addresses come from LOAD_TEXOBJ, as in Aurora's own renderer; J3D's display lists still
  // write console TMEM offsets and truncated pointers there.
  limits.dropped_bp.set(0x64);
  limits.dropped_bp.set(0x65);
  for (unsigned i = 0; i < 8; ++i) {
    limits.dropped_bp.set(0x94 + i);
    limits.dropped_bp.set(0xb4 + i);
  }
  for (unsigned i = 0; i < 4; ++i) {
    limits.filtered_bp.set(0x80 + i);
    limits.filtered_bp.set(0xa0 + i);
  }
}

void AuroraStream::feed(const std::uint8_t* data, std::size_t size) {
  if (!failure_.empty()) {
    return;
  }
  if (!initialized_) {
    initialized_ = true;
    // The GLES fork has no GXSetScissorBoxOffset, so nothing writes BP 0x59 there; the console's
    // field for offset 0 is 171 (342 halved), matching what GXSetScissor and GXSetViewport add.
    // Upstream trees write it themselves (see filter_bp) and replace this value.
    write_bp(0x59000000u | 171u << 10 | 171u);
  }
  ++stats_.drains;
  ++stats_.decoder_feeds;
  const auto status = decoder_.feed({data, size});
  if (!status) {
    fail("decoder: " + status.message);
  }
}

bool AuroraStream::host_command(starlet::gx::ByteView input, std::size_t& consumed) {
  if (input.data[0] >= 0x20 && input.data[0] <= 0x38) {
    consumed = 5;
    if (input.size < 5) {
      fail("truncated indexed XF load");
      return false;
    }
    auto& matrices = matrix_arrays_[(input.data[0] - 0x20) / 8];
    if (matrices.pending) {
      set_array(12 + (input.data[0] - 0x20) / 8, matrices.pointer, matrices.size, matrices.host_order);
      matrices.pending = false;
    }
    write(input.data, 5);
    return failure_.empty();
  }
  if (input.data[0] == 0x40) {
    // GXCallDisplayList copies the list into the FIFO; a nested call names a host pointer.
    consumed = 9;
    ++stats_.ignored;
    return true;
  }
  ++stats_.aurora_commands;
  data_ = input.data;
  size_ = input.size;
  std::size_t pos = 0;
  const bool ok = aurora_command(pos);
  data_ = nullptr;
  consumed = pos;
  return ok;
}

bool AuroraStream::filter_bp(std::uint8_t address, std::uint32_t& value) {
  if (address == 0x59) {
    // Upstream Aurora's GXSetScissorBoxOffset (GXInit and the game's own calls) encodes each half
    // as (offset - 342) >> 1 in ten bits; the console's field, which Starlet decodes, is
    // (offset + 342) >> 1. Rebuild the console field from the signed offset the write carries.
    // The GLES fork has no GXSetScissorBoxOffset and keeps the initial write in feed().
    const auto console_field = [](std::uint32_t field) {
      const int half = static_cast<int>(field & 0x3ff);
      return static_cast<std::uint32_t>((half >= 512 ? half - 1024 : half) + 342) & 0x3ff;
    };
    value = (value & ~0xfffffu) | console_field(value) | console_field(value >> 10) << 10;
    return true;
  }
  if (address == 0x52) {
    // Bit 14 selects a display copy, which GXCopyDisp writes with no destination of its own: the
    // frame is presented from the EFB, so only texture copies are planned.
    if (!(value & 0x4000)) {
      copy_texture(value);
    }
    return false;
  }
  if ((value >> 17) == 0x7f) {
    // Texture mode 0 with a negative LOD bias sign-extended over bits 17..23: the PC build
    // converts the float bias to an integer the console truncated to its 8-bit field.
    value &= 0x1ffff;
    ++stats_.repaired;
  }
  return true;
}

bool AuroraStream::aurora_command(std::size_t& pos) {
  if (size_ - pos < 3) {
    fail("truncated Aurora command");
    return false;
  }
  const std::uint8_t* command = data_ + pos;
  const std::uint16_t sub = be16(command + 1);
  const std::uint8_t* payload = command + 3;
  const std::size_t available = size_ - pos - 3;
  auto truncated = [&] {
    fail("truncated Aurora command " + std::to_string(sub));
    return false;
  };
  std::size_t length = 0;
  if (sub >= commands::arraybase && sub <= (commands::arraybase | 0x0f)) {
    if (available < (length = commands::arraybase_payload)) {
      return truncated();
    }
    const auto size = be32(payload + 8);
    const void* pointer = host_pointer(be64(payload));
    const bool host_order = payload[12] == 1;
    const unsigned slot_index = sub - commands::arraybase;
    if (slot_index >= 12) {
      // Matrix arrays are read only by indexed XF loads, which J3D sets far more often than
      // it uses: the base resolves when a load needs it.
      matrix_arrays_[slot_index - 12] = {pointer, size, host_order, true};
    } else {
      set_array(slot_index, pointer, size, host_order);
    }
  } else {
    // Fixed-size payloads are checked before they are read.
    if (commands::find_fixed(sub, length) && available < length) {
      return truncated();
    }
    switch (sub) {
    case commands::viewport_render:
    case commands::scissor_render:
    case commands::projection_full: ++stats_.ignored; break;
    case commands::debug_group_push:
    case commands::debug_marker_insert:
      if (available < 2) {
        return truncated();
      }
      length = 2 + std::size_t{be16(payload)};
      break;
    case commands::debug_group_pop: break;
    case commands::load_texobj: load_texture(payload); break;
    case commands::load_tlut: load_tlut(payload); break;
    case commands::destroy_texobj:
    case commands::destroy_tlut:
    case commands::destroy_copy_tex: break;
    case commands::load_copy_src: {
      const auto left = be32(payload), top = be32(payload + 4), width = be32(payload + 8), height = be32(payload + 12);
      copy_source_ = 0x49000000u | (top & 1023) << 10 | (left & 1023);
      copy_size_ = 0x4a000000u | ((height - 1) & 1023) << 10 | ((width - 1) & 1023);
      break;
    }
    case commands::load_copy_dst:
      set_copy_destination_format(be32(payload), be32(payload + 4), be32(payload + 8), payload[12] != 0);
      break;
    case commands::load_copy_dest: copy_destination_ = host_pointer(be64(payload)); break;
    case commands::begin_offscreen: begin_offscreen(be32(payload), be32(payload + 4)); break;
    case commands::end_offscreen: end_offscreen(); break;
    case commands::draw_sized:
      if (!draw_sized(payload, available, length)) {
        return false;
      }
      break;
    case commands::draw_indexed:
      if (!draw_indexed(payload, available, length)) {
        return false;
      }
      break;
    case list_source_command: {
      length = 20;
      if (available < length) {
        return truncated();
      }
      const auto bytes = be32(payload + 8);
      if (bytes <= available - 20) {
        memory_.note_fifo_source(payload + 20, bytes, host_pointer(be64(payload)), be64(payload + 12));
      }
      break;
    }
    case commands::polygon_offset:
      ++stats_.ignored;
      issues::report("polygon offset not applied: coplanar decals may z-fight");
      break;
#ifdef GX_AURORA_SET_TEX_IMG_PTR
    case commands::set_tex_img_ptr: set_texture_image_pointer(payload); break;
#endif
#ifdef GX_AURORA_LOAD_DISP_COPY_SRC
    // Display copies present the EFB itself; the source rectangle only matters to Aurora's XFB.
    case commands::load_disp_copy_src: ++stats_.ignored; break;
#endif
#ifdef GX_AURORA_SET_DRAWS_BLOCKING
    case commands::set_draws_blocking: ++stats_.ignored; break;
#endif
#ifdef GX_AURORA_READBACK_CAPTURE_PEEKS
    case commands::readback_peeks:
#endif
#ifdef GX_AURORA_READBACK_CAPTURE_COPIES
    case commands::readback_copies:
#endif
#if defined(GX_AURORA_READBACK_CAPTURE_PEEKS) || defined(GX_AURORA_READBACK_CAPTURE_COPIES)
      if (available < commands::readback_header) {
        return truncated();
      }
      // Starlet's copies are GPU-resident and its EFB peeks are the game's own; Aurora's
      // asynchronous readback streams are not implemented, and nothing answers them.
      length = commands::readback_header;
      if (sub == commands::readback_copies) {
        length += std::size_t{be32(payload + 16)} * commands::readback_copy_entry;
      } else {
        length += 1 + std::size_t{be32(payload + 16)} * commands::readback_peek_entry;
      }
      ++stats_.ignored;
      issues::report("Aurora readback capture not supported: AuroraReadbackGet never receives these results");
      break;
#endif
#ifdef GX_AURORA_CLEAR
    case commands::clear: clear_efb(payload); break;
#endif
#ifdef GX_AURORA_REQUEST_DEPTH_SNAPSHOT
    case commands::request_depth_snapshot:
      ++stats_.ignored;
      issues::report("depth snapshot not produced: effects that read depth back see stale values");
      break;
#endif
    default: fail("unknown Aurora command " + std::to_string(sub)); return false;
    }
  }
  if (length > available) {
    return truncated();
  }
  pos += 3 + length;
  return failure_.empty();
}

void AuroraStream::set_array(unsigned slot_index, const void* pointer, std::uint32_t size, bool host_order) {
  auto& slot = array_slots_[slot_index];
  std::uint32_t address = slot.address;
  // Most loads repeat the slot's array; the same arguments map and describe it the same way.
  if (!(address && slot.pointer == pointer && slot.size == size && slot.host_order == host_order &&
        slot.epoch == memory_.epoch())) {
    address = memory_.address(pointer, size);
    if (!address) {
      ++stats_.unmapped;
      static constexpr std::string_view kind = "vertex array outside mapped memory: draws reading it get wrong vertices";
      if (pointer && size) {
        if (const auto when = issues::note(kind); when != issues::Occurrence::quiet) {
          issues::write(kind, when, &decoder_.state(), fmt::format("slot {} size {}", slot_index, size));
        }
      }
    }
    memory_.set_array(address, size, host_order);
    slot = {pointer, size, address, host_order, memory_.epoch()};
  }
  const auto cp = static_cast<std::uint8_t>(0xa0 + slot_index);
  if (decoder_.state().cp(cp) != address) {
    write_cp(cp, address);
  }
}

void AuroraStream::load_texture(const std::uint8_t* payload) {
  const std::uint8_t map = payload[0];
  const auto width = be32(payload + 9), height = be32(payload + 13), format = be32(payload + 17);
  const auto tlut = be32(payload + 21);
  // A repeated load of the same texture object maps and describes it the same way: only
  // registers something else rewrote since need writing again.
  const auto tlut_name = std::min<std::uint32_t>(tlut, 19);
  if (map < 8) {
    auto& slot = texture_slots_[map];
    if (slot.valid && slot.epoch == memory_.epoch() && slot.copies == copy_epoch_ &&
        slot.tlut_format == tlut_formats_[tlut_name] && std::memcmp(slot.payload.data(), payload, slot.payload.size()) == 0) {
      for (const auto word : slot.writes) {
        if (word && decoder_.state().bp(static_cast<std::uint8_t>(word >> 24)) != (word & 0xffffff)) {
          write_bp(word);
        }
      }
      decoder_.set_texture_extent(map, slot.extent);
      return;
    }
    slot.valid = false;
  }
  const bool mips = payload[25] != 0;
  const void* pointer = host_pointer(be64(payload + 1));
  auto size = (format & _GX_TF_PC) ? 0 : GXGetTexBufferSize(static_cast<u16>(width), static_cast<u16>(height), format, mips, 11);
  // Aurora's PC formats hold linear data where the image register (the format's low four
  // bits) reads GX tiles, so load a tiled copy of each.
  if (format & _GX_TF_PC) {
    std::size_t (*tiled_size)(std::uint32_t, std::uint32_t) = nullptr;
    void (*tile)(const std::uint8_t*, std::uint32_t, std::uint32_t, std::uint8_t*) = nullptr;
    switch (format) {
    case GX_TF_R8_PC: tiled_size = i8_tiled_size; tile = tile_i8; break;
    case GX_TF_RGBA8_PC: tiled_size = rgba8_tiled_size; tile = tile_rgba8; break;
    case GX_TF_BC1_PC: tiled_size = cmpr_tiled_size; tile = tile_bc1_as_cmpr; break;
    default: break;
    }
    if (!tile || mips || map >= 8 || !width || !height) {
      issues::report(tile ? "mipmapped or invalid PC texture not loaded: draws on this map sample the previous texture"
                          : "PC texture format has no GX equivalent: draws on this map sample the previous texture",
                     &decoder_.state(), fmt::format("map {} format {:#x} {}x{} mips {}", map, format, width, height, mips));
      ++stats_.unmapped;
      return;
    }
    size = static_cast<decltype(size)>(tiled_size(width, height));
    auto& tiles = linear_tiles_[map];
    tiles.resize(std::size_t{size} + 31);
    auto* aligned = reinterpret_cast<std::uint8_t*>((reinterpret_cast<std::uintptr_t>(tiles.data()) + 31) & ~std::uintptr_t{31});
    tile(static_cast<const std::uint8_t*>(pointer), width, height, aligned);
    pointer = aligned;
    memory_.note_write(pointer, size);
  }
  const auto address = memory_.address(pointer, size, true);
  if (!address || (address & 31) || map >= 8) {
    issues::report("texture not loaded: draws on this map sample the previous texture", &decoder_.state(),
                   fmt::format("map {} format {} {}x{} size {} address {:x}", map, format, width, height, size, address));
    ++stats_.unmapped;
    return;
  }
  auto& slot = texture_slots_[map];
  slot.writes = {std::uint32_t{texture_register(0x94, 0xb4, map)} << 24 | address >> 5, 0, 0};
  write_bp(slot.writes[0]);
  memory_.note_texture(pointer, size, be32(payload + 30));
  // Aurora's textures may pass GX's 1024 texels (a font's pages joined into one, for example); the
  // image register keeps only the low ten bits of each size.
  slot.extent = {};
  if (width > 1024 || height > 1024) {
    slot.extent = {static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height)};
  }
  decoder_.set_texture_extent(map, slot.extent);
  if (const auto copy = copy_shapes_.find(pointer); copy != copy_shapes_.end() && !mips && copy->second.format == format &&
                                                    (copy->second.width != width || copy->second.height != height)) {
    slot.writes[1] = std::uint32_t{texture_register(0x88, 0xa8, map)} << 24 | std::uint32_t{format} << 20 |
                     std::uint32_t(copy->second.height - 1) << 10 | std::uint32_t(copy->second.width - 1);
    write_bp(slot.writes[1]);
    ++stats_.resized_copies;
  }
  static const bool trace = [] {
    const char* value = std::getenv("AURORA_STARLET_TRACE");
    return value && *value;
  }();
  static unsigned traced = 0;
  if (trace && traced < 24) {
    const auto& state = decoder_.state();
    const unsigned base = (map < 4 ? 0x80 : 0xa0) + map % 4;
    std::fprintf(stderr, "[starlet-trace] texobj map %u fmt %u %ux%u mode0 %06x mode1 %06x image0 %06x image1 %06x image2 %06x image3 %06x ptr %llx\n",
                 map, format, width, height, state.bp(base), state.bp(base + 4), state.bp(base + 8), state.bp(base + 12),
                 state.bp(base + 16), state.bp(base + 20), static_cast<unsigned long long>(be64(payload + 1)));
    ++traced;
  }
  if (format == GX_TF_C4 || format == GX_TF_C8 || format == GX_TF_C14X2) {
    slot.writes[2] = std::uint32_t{texture_register(0x98, 0xb8, map)} << 24 | std::uint32_t{tlut_formats_[tlut_name]} << 10 |
                     tlut_line(tlut_name);
    write_bp(slot.writes[2]);
  }
  std::memcpy(slot.payload.data(), payload, slot.payload.size());
  slot.epoch = memory_.epoch();
  slot.copies = copy_epoch_;
  slot.tlut_format = tlut_formats_[tlut_name];
  slot.valid = failure_.empty() && !(format & _GX_TF_PC); // The tiled copy follows the source each load.
}

void AuroraStream::load_tlut(const std::uint8_t* payload) {
  const std::uint8_t name = payload[0];
  const auto format = be32(payload + 9);
  const std::size_t bytes = std::size_t{be16(payload + 13)} * 2;
  const void* pointer = host_pointer(be64(payload + 1));
  const auto address = memory_.address(pointer, bytes, true);
  if (!address || (address & 31) || name >= tlut_formats_.size()) {
    issues::report("TLUT not loaded: palette textures keep the previous palette", nullptr,
                   fmt::format("tlut {} format {} bytes {} address {:x}", name, format, bytes, address));
    ++stats_.unmapped;
    return;
  }
  tlut_formats_[name] = static_cast<std::uint8_t>(format & 3);
  const auto control = static_cast<std::uint32_t>((bytes + 31) / 32) << 10 | tlut_line(name);
  // BP 0x64 holds 21 bits of 32-byte units; palettes the address space places above 64 MiB
  // (window ranges, or a large arena's upper part) load from their bytes instead.
  if (address >> 5 >= 1u << 21) {
    const auto* palette = static_cast<const std::uint8_t*>(pointer);
    const std::size_t loaded = (bytes + 31) / 32 * 32;
    if (loaded != bytes) {
      tlut_bytes_.assign(palette, palette + bytes);
      tlut_bytes_.resize(loaded);
      palette = tlut_bytes_.data();
    }
    const auto status = decoder_.load_tlut(control, {palette, loaded});
    if (!status) {
      fail("decoder: " + status.message);
    }
    return;
  }
  write_bp(0x64000000u | address >> 5);
  write_bp(0x65000000u | control);
}

void AuroraStream::set_copy_destination_format(std::uint32_t width, std::uint32_t height, std::uint32_t format,
                                               bool mipmap) {
  (void)height;
  std::uint32_t pe_format = format & 0xf;
  if (format == GX_TF_Z16) {
    pe_format = 0xb;
  }
  const bool intensity = format == GX_TF_I4 || format == GX_TF_I8 || format == GX_TF_IA4 || format == GX_TF_IA8 ||
                         format == GX_CTF_YUVA8;
  copy_depth_ = (format & _GX_TF_ZTF) == _GX_TF_ZTF;
  copy_command_ = (pe_format & 8) | (pe_format & 7) << 4 | (mipmap ? 1u : 0u) << 9 | (intensity ? 3u : 2u) << 15;
  std::uint32_t tile_width = 4, blocks = 1;
  copy_tile(format, tile_width, blocks);
  copy_stride_ = 0x4d000000u | (((width + tile_width - 1) / tile_width) * blocks & 1023);
}

void AuroraStream::copy_texture(std::uint32_t command) {
  if (!copy_destination_) {
    issues::report("EFB copy without a destination: copy dropped");
    ++stats_.ignored;
    return;
  }
  const bool clear = command & 0x800;
  starlet::gx::TextureCopyRegisters registers{copy_source_, copy_size_, copy_stride_,
                                              0x52000000u | copy_command_ | (clear ? 0x800u : 0u), copy_depth_};
  starlet::gx::TextureCopy plan;
  try {
    plan = starlet::gx::plan_texture_copy(decoder_.state(), registers, clear);
  } catch (const std::exception& e) {
    ++stats_.failed_copies;
    issues::report(fmt::format("EFB copy refused: {}", e.what()), &decoder_.state(),
                   fmt::format("src {:06x} size {:06x} stride {:06x} cmd {:06x} depth {} pixel {:06x}", copy_source_ & 0xffffff,
                               copy_size_ & 0xffffff, copy_stride_ & 0xffffff, registers.command & 0xffffff,
                               copy_depth_ ? 1 : 0, decoder_.state().bp(0x43)));
    return;
  }
  const auto bytes = starlet::gx::texture_copy_bytes(plan);
  const auto target = memory_.copy_target(copy_destination_, bytes);
  if (!target.address) {
    issues::report("EFB copy destination outside mapped memory: copy dropped", nullptr, fmt::format("bytes {}", bytes));
    ++stats_.unmapped;
    return;
  }
  if (target.replaced) {
    efb_.release(target.replaced, target.replaced_size);
  }
  efb_.release(target.address, target.size);
  const CopyShape shape{static_cast<std::uint16_t>(plan.destination_width()), static_cast<std::uint16_t>(plan.destination_height()),
                        static_cast<std::uint8_t>(starlet::gx::texture_copy_sample_format(plan))};
  auto& known = copy_shapes_[copy_destination_];
  if (known.width != shape.width || known.height != shape.height || known.format != shape.format) {
    known = shape;
    ++copy_epoch_;
  }
  efb_.copy_texture(plan, target.storage, bytes, target.address);
  ++stats_.copies;
}

// GDSetTexImgPtr: the image address of a map whose size and format the stream wrote as registers.
void AuroraStream::set_texture_image_pointer(const std::uint8_t* payload) {
  const std::uint8_t map = payload[0];
  const void* pointer = host_pointer(be64(payload + 1));
  if (map >= 8) {
    fail("texture image pointer for map " + std::to_string(map));
    return;
  }
  const auto& state = decoder_.state();
  const auto image = state.bp(texture_register(0x88, 0xa8, map));
  const auto mode = state.bp(texture_register(0x80, 0xa0, map));
  const auto width = (image & 1023) + 1, height = ((image >> 10) & 1023) + 1, format = (image >> 20) & 15;
  const bool mips = ((mode >> 5) & 3) != 0;
  const auto size = GXGetTexBufferSize(static_cast<u16>(width), static_cast<u16>(height), format, mips, 11);
  texture_slots_[map].valid = false;
  const auto address = memory_.address(pointer, size, true);
  if (!address || (address & 31)) {
    issues::report("texture image pointer not mapped: draws on this map sample the previous texture", &decoder_.state(),
                   fmt::format("map {} format {} {}x{} size {} address {:x}", map, format, width, height, size, address));
    ++stats_.unmapped;
    return;
  }
  write_bp(std::uint32_t{texture_register(0x94, 0xb4, map)} << 24 | address >> 5);
  memory_.note_texture(pointer, size, 0);
}

// GXClear and GXClearSubrect: a clear of the EFB that is not tied to a copy.
void AuroraStream::clear_efb(const std::uint8_t* payload) {
  const auto flags = payload[0];
  const auto& state = decoder_.state();
  const int height = state.efb().height, width = starlet::gx::EfbLayout::width;
  int x = 0, y = 0, w = width, h = height;
  if (!(flags & 0x08)) {
    // Rectangle in EFB coordinates (y down); a clear is not scissored.
    const auto left = be32(payload + 1), top = be32(payload + 5), cw = be32(payload + 9), ch = be32(payload + 13);
    x = static_cast<int>(std::min<std::uint32_t>(left, width));
    y = static_cast<int>(std::min<std::uint32_t>(top, height));
    w = static_cast<int>(std::min<std::uint32_t>(cw, width - x));
    h = static_cast<int>(std::min<std::uint32_t>(ch, height - y));
    if (w <= 0 || h <= 0) {
      return;
    }
  }
  starlet::gx::EfbClear plan;
  plan.efb = state.efb();
  plan.rectangle = {x, height - y - h, w, h};
  plan.rgba6 = (state.bp(0x43) & 7) == 1;
  plan.color_write = flags & 0x01;
  plan.alpha_write = plan.rgba6 && (flags & 0x02);
  plan.depth_write = flags & 0x04;
  plan.depth = be32(payload + 21) & 0xffffff;
  std::array<std::uint8_t, 4> color{payload[17], payload[18], payload[19], payload[20]};
  if (plan.rgba6) {
    for (auto& channel : color) {
      const unsigned code = channel >> 2;
      channel = static_cast<std::uint8_t>(code << 2 | code >> 4);
    }
  } else {
    color[3] = 255;
  }
  plan.tile.fill(color);
  efb_.clear(plan);
  ++stats_.clears;
}

bool AuroraStream::draw_sized(const std::uint8_t* payload, std::size_t available, std::size_t& consumed) {
  const std::uint8_t command = payload[0];
  const auto bytes = be32(payload + 1);
  consumed = 5 + std::size_t{bytes};
  if (consumed > available) {
    fail("truncated sized draw");
    return false;
  }
  std::size_t vertex = 0;
  if (!vertex_size(command & 7, vertex) || !vertex) {
    fail("sized draw with an empty vertex layout");
    return false;
  }
  const std::size_t count = bytes / vertex;
  if (count > 0xffff) {
    fail("sized draw beyond 65535 vertices");
    return false;
  }
  scratch_.resize(3 + count * vertex);
  scratch_[0] = command;
  scratch_[1] = static_cast<std::uint8_t>(count >> 8);
  scratch_[2] = static_cast<std::uint8_t>(count);
  std::memcpy(scratch_.data() + 3, payload + 5, count * vertex);
  write(scratch_.data(), scratch_.size());
  ++stats_.expanded_draws;
  return failure_.empty();
}

bool AuroraStream::draw_indexed(const std::uint8_t* payload, std::size_t available, std::size_t& consumed) {
  const std::uint8_t command = payload[0];
  const std::size_t vertices = be16(payload + 1);
  const std::size_t indices = be32(payload + 3);
  std::size_t vertex = 0;
  if (!vertex_size(command & 7, vertex)) {
    return false;
  }
  const std::uint8_t* index_data = payload + 7;
  const std::uint8_t* vertex_data = index_data + indices * 2;
  consumed = 7 + indices * 2 + vertices * vertex;
  if (consumed > available) {
    fail("truncated indexed draw");
    return false;
  }
  // Indices are host-endian; expand them into plain triangles, 21845 at most per draw.
  for (std::size_t first = 0; first < indices; first += 65535) {
    const std::size_t count = std::min<std::size_t>(indices - first, 65535);
    scratch_.resize(3 + count * vertex);
    scratch_[0] = static_cast<std::uint8_t>(0x90 | (command & 7));
    scratch_[1] = static_cast<std::uint8_t>(count >> 8);
    scratch_[2] = static_cast<std::uint8_t>(count);
    for (std::size_t i = 0; i < count; ++i) {
      std::uint16_t index;
      std::memcpy(&index, index_data + (first + i) * 2, 2);
      if (index >= vertices) {
        fail("indexed draw names a vertex it does not carry");
        return false;
      }
      std::memcpy(scratch_.data() + 3 + i * vertex, vertex_data + std::size_t{index} * vertex, vertex);
    }
    write(scratch_.data(), scratch_.size());
  }
  ++stats_.expanded_draws;
  return failure_.empty();
}

} // namespace aurora::starlet_backend
