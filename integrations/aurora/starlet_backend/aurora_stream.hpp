#pragma once

#include "host_memory.hpp"

#include "starlet/gx/decoder.hpp"
#include "starlet/gx/texture_copy.hpp"

#include <array>
#include <optional>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace aurora::starlet_backend {

// The bridge's own Aurora subcommand, followed by a 64-bit display-list pointer, a u32 size and
// the list's u64 write stamp when copied (note_display_list): the next size bytes of the FIFO
// are that copy.
constexpr std::uint16_t list_source_command = 0x0f00;

// Receives the EFB work the stream asks for beyond draws: texture copies and offscreen passes.
class EfbTarget {
public:
  virtual ~EfbTarget() = default;
  virtual void copy_texture(const starlet::gx::TextureCopy&, std::uint8_t* destination, std::size_t capacity,
                            std::uint32_t address) = 0;
  // Drops earlier copies in the range: a copy replaces the previous one at its destination.
  virtual void release(std::uint32_t address, std::size_t size) = 0;
  // GXCreateFrameBuffer and GXRestoreFrameBuffer: draws and copies between them use a separate
  // cleared target, and the EFB is left as it was.
  virtual void begin_offscreen() = 0;
  virtual void end_offscreen() = 0;
};

struct StreamStats {
  std::uint64_t aurora_commands = 0, copies = 0, expanded_draws = 0, decoder_feeds = 0, drains = 0;
  std::uint64_t ignored = 0, unmapped = 0, repaired = 0, failed_copies = 0, resized_copies = 0;
};

// Translates Aurora's GX command stream into the hardware stream Starlet decodes.
// Hardware commands pass through unchanged. Aurora's extensions carry host pointers and
// arguments the console kept in registers; they become the register writes they stand
// for: CP array bases, texture image and TLUT addresses, TLUT loads, and copy registers.
// Texture copies run through Starlet's planner; offscreen passes go to the EFB target.
class AuroraStream {
public:
  AuroraStream(HostMemory& memory, starlet::gx::Decoder& decoder, EfbTarget& efb);
  // The host opcodes and BP addresses the decoder hands to host_command and filter_bp.
  static void configure(starlet::gx::Limits& limits);
  // One drained FIFO buffer, big-endian, starting at a command boundary.
  void feed(const std::uint8_t* data, std::size_t size);
  // CommandSink::host_command and filter_bp for the decoder's sink.
  bool host_command(starlet::gx::ByteView input, std::size_t& consumed);
  bool filter_bp(std::uint8_t address, std::uint32_t& value);
  const StreamStats& stats() const { return stats_; }
  // Development: logs every texture copy while set.
  // Set when the stream could not be followed; later buffers are skipped.
  const std::string& failure() const { return failure_; }

private:
  void write(const std::uint8_t* bytes, std::size_t size);
  void write_cp(std::uint8_t address, std::uint32_t value);
  void write_bp(std::uint32_t word);
  void write_xf(std::uint16_t address, const std::uint32_t* values, std::size_t count);
  void begin_offscreen(std::uint32_t width, std::uint32_t height);
  void end_offscreen();
  bool vertex_size(std::uint8_t vat, std::size_t& size);
  bool aurora_command(std::size_t& pos);
  void set_array(unsigned slot_index, const void* pointer, std::uint32_t size, bool host_order);
  void load_texture(const std::uint8_t* payload);
  void load_tlut(const std::uint8_t* payload);
  void set_copy_destination_format(std::uint32_t width, std::uint32_t height, std::uint32_t format, bool mipmap);
  void copy_texture(std::uint32_t command);
  bool draw_sized(const std::uint8_t* payload, std::size_t available, std::size_t& consumed);
  bool draw_indexed(const std::uint8_t* payload, std::size_t available, std::size_t& consumed);
  void fail(std::string message);

  HostMemory& memory_;
  starlet::gx::Decoder& decoder_;
  EfbTarget& efb_;
  StreamStats stats_;
  std::string failure_;
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0; // The host command being handled.
  std::array<std::uint8_t, 20> tlut_formats_{};
  // Texture-copy registers as GXSetTexCopySrc/Dst would have written them.
  std::uint32_t copy_source_ = 0x49000000, copy_size_ = 0x4a000000, copy_stride_ = 0x4d000000, copy_command_ = 0;
  bool copy_depth_ = false;
  const void* copy_destination_ = nullptr;
  // The last copy into each destination. Aurora samples a copy as made, whatever size a texture
  // object gives it, and PC code can rely on it: it copies the frame at full size into
  // a buffer it still describes at half size.
  struct CopyShape {
    std::uint16_t width = 0, height = 0;
    std::uint8_t format = 0;
  };
  std::unordered_map<const void*, CopyShape> copy_shapes_;
  std::uint64_t copy_epoch_ = 0; // Advances when a copy's shape changes.
  // The last texture object each map loaded and the BP words that load wrote.
  struct TextureSlot {
    std::array<std::uint8_t, 34> payload{};
    std::array<std::uint32_t, 3> writes{}; // Image address, resized image size, TLUT; zero when unused.
    starlet::gx::TextureExtent extent{};
    std::uint64_t epoch = 0, copies = 0;
    std::uint8_t tlut_format = 0;
    bool valid = false;
  };
  std::array<TextureSlot, 8> texture_slots_{};
  std::array<std::vector<std::uint8_t>, 8> linear_tiles_; // Tiled GX copies of PC-format loads per map.
  std::vector<std::uint8_t> scratch_;
  // The last array each CP array slot was given, valid while the memory epoch is unchanged.
  struct ArraySlot {
    const void* pointer = nullptr;
    std::uint32_t size = 0, address = 0;
    bool host_order = false;
    std::uint64_t epoch = 0;
  };
  std::array<ArraySlot, 16> array_slots_{};
  // The latest matrix arrays (slots 12-15) Aurora named, applied by the next indexed XF load.
  struct MatrixArray {
    const void* pointer = nullptr;
    std::uint32_t size = 0;
    bool host_order = false, pending = false;
  };
  std::array<MatrixArray, 4> matrix_arrays_{};
  // Vertex strides by VAT, valid while the decoder's CP layout revision is unchanged.
  std::array<std::pair<std::uint64_t, std::size_t>, 8> strides_{};
  bool initialized_ = false;
  // The viewport (XF 0x101a-0x101f) and scissor (BP 0x20, 0x21) an offscreen pass replaces.
  struct SuspendedView {
    std::array<std::uint32_t, 6> viewport{};
    std::uint32_t scissor0 = 0, scissor1 = 0;
  };
  std::optional<SuspendedView> suspended_view_;
};

} // namespace aurora::starlet_backend
