#pragma once

#include "starlet/gx/decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aurora::starlet_backend {

// Gives host pointers the 32-bit GX addresses Starlet's decoder works in. The game's MEM1
// arena maps linearly into [0, 512 MiB); other ranges get addresses from a second window
// as Aurora's command stream names them. Every address keeps its pointer's alignment
// modulo 32, which GX texture and TLUT registers rely on.
class HostMemory final : public starlet::gx::MemoryReader {
public:
  HostMemory(const void* arena, std::size_t arena_size);

  // A GX address for [pointer, pointer + size); 0 for null or when no address space is left.
  // aligned asks for a 32-byte aligned address, as texture and TLUT registers hold them;
  // a pointer that is not 32-byte aligned then gets one from the window.
  std::uint32_t address(const void* pointer, std::size_t size, bool aligned = false);
  // An EFB copy's destination. Aurora keeps each copy as a texture of its own, named by the
  // destination pointer, so every destination gets a private address range and storage of
  // its own: copies never write game memory or overlap each other, and a texture whose data
  // pointer is a copy destination reads that range. replaced names a range given up because
  // the copy outgrew it.
  struct CopyTarget {
    std::uint32_t address = 0;
    std::size_t size = 0;
    std::uint8_t* storage = nullptr;
    std::uint32_t replaced = 0;
    std::size_t replaced_size = 0;
  };
  CopyTarget copy_target(const void* pointer, std::size_t size);
  // Records how the array at a GX address encodes its numbers (Aurora's `le` flag).
  void set_array(std::uint32_t address, std::size_t size, bool host_order);
  // A drained FIFO buffer is about to be decoded. The game does not run while one is, so
  // memory read during a feed is fixed: host-order arrays, written by PC code that reports
  // no writes, are hashed once per feed and their generation advances when the hash changes.
  void begin_feed() {
    ++feed_;
    fifo_sources_.clear();
  }
  // During a feed: [fifo, fifo + size) of the drained buffer copies the arena bytes at source,
  // taken when their write stamp was stamp.
  void note_fifo_source(const std::uint8_t* fifo, std::size_t size, const void* source, std::uint64_t stamp);
  bool in_arena(const void* pointer, std::size_t size) const;
  // Any thread: the latest write report touching an arena range; equal stamps of one range
  // mean no write was reported between them.
  std::uint64_t stamp(const void* pointer, std::size_t size) const;
  // The arena bytes a FIFO copy came from, named with the stamp they were copied at; other
  // bytes keep the default identity.
  SourceIdentity source_identity(const void* bytes, std::size_t size) override;
  // The game's cache store or flush of [pointer, pointer + size): advances the write generation
  // of every big-endian array and loaded texture the range touches.
  void note_write(const void* pointer, std::size_t size);
  // A texture load of [pointer, pointer + size) with the texture object's data version: the
  // generation advances when the version changes (GXInitTexObjData) or the range grows.
  void note_texture(const void* pointer, std::size_t size, std::uint32_t version);
  // The texture generation at pointer, zero when no load names it (Starlet then hashes it).
  std::uint64_t source_generation(const void* pointer) override;
  // Snapshots of textures written this frame reuse this pool every frame; they keep no generation.
  void set_snapshot_pool(const std::uint8_t* pool, std::size_t size) { pool_ = pool; pool_size_ = size; }
  // Null when the address is unmapped.
  std::uint8_t* pointer(std::uint32_t address, std::size_t size) const;
  // Forgets addresses outside the arena; their pointers may be reused by new allocations.
  void reset_window();
  // Gives [data, data + size) one linear range of addresses, so pointers anywhere inside it
  // map without registering each one. Once, before any address() call names it.
  void map_region(const std::uint8_t* data, std::size_t size);
  bool copy_destination(const void* pointer) const { return copy_targets_.count(pointer) != 0; }
  // Advances whenever an address or array description could change: a new copy target, a
  // window reset, or an array redescribed. Results cached under one epoch stay valid.
  std::uint64_t epoch() const { return epoch_; }

  bool copy(std::uint32_t address, std::uint8_t* destination, std::size_t size) override;
  starlet::gx::ByteView view(std::uint32_t address, std::size_t size) override;
  starlet::gx::MemoryArrayView array_view(std::uint32_t address) override;

private:
  struct Range {
    const std::uint8_t* data;
    std::size_t size;
  };
  struct Array {
    std::size_t size;
    bool host_order;
    const std::uint8_t* data;
    std::uint64_t generation;
    std::uint64_t digest = 0, feed = 0; // Host-order content hash and the feed it was taken in.
  };
  // Host-order arrays larger than this keep generation zero.
  static constexpr std::size_t max_digest_bytes = 8 << 20;
  // Addresses below arena_base stay invalid, so a null or unset GX address reads nothing. The
  // arena takes addresses up to window_base (a 256 MiB MEM1 fits); GX texture
  // registers hold 29-bit addresses, so the window ends at 512 MiB.
  static constexpr std::uint32_t arena_base = 0x1000, window_base = 0x10002000, window_end = 0x20000000;
  const std::uint8_t* arena_;
  std::size_t arena_size_;
  std::uint32_t arena_bias_;
  std::uint32_t next_ = window_base;
  const std::uint8_t* region_ = nullptr;
  std::size_t region_size_ = 0;
  std::uint32_t region_address_ = 0;
  std::map<std::uint32_t, Range> window_;                // By GX address.
  std::unordered_map<const void*, std::uint32_t> named_; // Window address by pointer.
  std::unordered_map<std::uint32_t, Array> arrays_;
  std::map<const std::uint8_t*, std::uint32_t> arrays_by_pointer_; // Big-endian arrays.
  std::size_t largest_array_ = 0;
  struct TextureRange {
    std::size_t size = 0;
    std::uint32_t version = 0;
    std::uint64_t generation = 0;
  };
  std::map<const std::uint8_t*, TextureRange> textures_; // By data pointer.
  std::size_t largest_texture_ = 0;
  const std::uint8_t* pool_ = nullptr;
  std::size_t pool_size_ = 0;
  std::uint64_t writes_ = 1;
  // Write stamps of the arena in 1 KiB blocks; any thread's write report advances them.
  static constexpr unsigned block_bits = 10;
  std::unique_ptr<std::atomic<std::uint64_t>[]> block_stamps_;
  std::atomic<std::uint64_t> block_clock_{0};
  struct FifoSource {
    const std::uint8_t* fifo;
    std::size_t size;
    const std::uint8_t* source;
    std::uint64_t stamp;
  };
  std::vector<FifoSource> fifo_sources_; // In FIFO order.
  std::uint64_t epoch_ = 1;
  std::uint64_t feed_ = 1;
  std::mutex mutex_;
  struct CopyStorage {
    std::uint32_t address = 0;
    std::size_t size = 0;
    std::unique_ptr<std::uint8_t[]> bytes; // 32 bytes of slack for alignment.
    std::uint8_t* data() const;
  };
  std::uint32_t reserve(std::size_t size);
  std::unordered_map<const void*, CopyStorage> copy_targets_; // By destination pointer.
  std::map<std::uint32_t, const CopyStorage*> copy_addresses_;
};

} // namespace aurora::starlet_backend
