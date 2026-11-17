#include "host_memory.hpp"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <xxhash.h>

namespace aurora::starlet_backend {

HostMemory::HostMemory(const void* arena, std::size_t arena_size)
: arena_(static_cast<const std::uint8_t*>(arena))
, arena_size_(arena_size)
, arena_bias_(arena_base + static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(arena) & 31)) {
  if (arena_size_ + arena_bias_ > window_base) {
    arena_size_ = window_base - arena_bias_;
  }
  block_stamps_.reset(new std::atomic<std::uint64_t>[(arena_size_ >> block_bits) + 1]());
}

bool HostMemory::in_arena(const void* pointer, std::size_t size) const {
  const auto* p = static_cast<const std::uint8_t*>(pointer);
  return p >= arena_ && size <= arena_size_ && static_cast<std::size_t>(p - arena_) <= arena_size_ - size;
}

void HostMemory::note_fifo_source(const std::uint8_t* fifo, std::size_t size, const void* source, std::uint64_t stamp) {
  if (size && in_arena(source, size) && (fifo_sources_.empty() || fifo_sources_.back().fifo < fifo)) {
    fifo_sources_.push_back({fifo, size, static_cast<const std::uint8_t*>(source), stamp});
  }
}

std::uint64_t HostMemory::stamp(const void* pointer, std::size_t size) const {
  const auto offset = static_cast<std::size_t>(static_cast<const std::uint8_t*>(pointer) - arena_);
  std::uint64_t latest = 0;
  for (auto b = offset >> block_bits; b <= (offset + size - 1) >> block_bits; ++b) {
    latest = std::max(latest, block_stamps_[b].load(std::memory_order_relaxed));
  }
  return latest;
}

starlet::gx::MemoryReader::SourceIdentity HostMemory::source_identity(const void* bytes, std::size_t size) {
  const auto* p = static_cast<const std::uint8_t*>(bytes);
  auto it = std::upper_bound(fifo_sources_.begin(), fifo_sources_.end(), p,
                             [](const std::uint8_t* at, const FifoSource& s) { return at < s.fifo; });
  if (it != fifo_sources_.begin()) {
    --it;
    if (p + size <= it->fifo + it->size) {
      return {it->source + (p - it->fifo), it->stamp + 1};
    }
  }
  return {bytes, source_generation(bytes)};
}

std::uint32_t HostMemory::address(const void* pointer, std::size_t size, bool aligned) {
  if (!pointer) {
    return 0;
  }
  if (const auto copy = copy_targets_.find(pointer); copy != copy_targets_.end()) {
    return copy->second.address;
  }
  const auto* p = static_cast<const std::uint8_t*>(pointer);
  if (region_ && p >= region_ && p < region_ + region_size_ && size <= std::size_t(region_ + region_size_ - p)) {
    return region_address_ + static_cast<std::uint32_t>(p - region_);
  }
  const bool misaligned = aligned && (reinterpret_cast<std::uintptr_t>(p) & 31);
  if (!misaligned && p >= arena_ && p < arena_ + arena_size_ && size <= std::size_t(arena_ + arena_size_ - p)) {
    return static_cast<std::uint32_t>(p - arena_) + arena_bias_;
  }
  const auto found = named_.find(pointer);
  if (found != named_.end()) {
    auto& range = window_.at(found->second);
    if (range.size < size) {
      range.size = size;
    }
    return found->second;
  }
  static const bool trace = [] {
    const char* value = std::getenv("AURORA_STARLET_TRACE");
    return value && *value;
  }();
  if (trace) {
    std::fprintf(stderr, "[starlet-trace] window %p size %zu arena %p..%p\n", pointer, size,
                 static_cast<const void*>(arena_), static_cast<const void*>(arena_ + arena_size_));
  }
  const std::uint32_t alignment = misaligned ? 0 : reinterpret_cast<std::uintptr_t>(pointer) & 31;
  std::uint32_t start = (next_ + 31) & ~31u;
  start += alignment;
  if (size > window_end - start) {
    return 0;
  }
  window_[start] = {p, size};
  named_[pointer] = start;
  next_ = start + static_cast<std::uint32_t>(size);
  return start;
}

void HostMemory::map_region(const std::uint8_t* data, std::size_t size) {
  const std::uint32_t start = (next_ + 31) & ~31u;
  if (!data || reinterpret_cast<std::uintptr_t>(data) & 31 || size > window_end - start) {
    return;
  }
  ++epoch_;
  region_ = data;
  region_size_ = size;
  region_address_ = start;
  window_[start] = {data, size};
  next_ = start + static_cast<std::uint32_t>(size);
}

std::uint8_t* HostMemory::CopyStorage::data() const {
  const auto at = reinterpret_cast<std::uintptr_t>(bytes.get());
  return bytes.get() + ((32 - at % 32) % 32);
}

std::uint32_t HostMemory::reserve(std::size_t size) {
  const std::uint32_t start = (next_ + 31) & ~31u;
  if (size > window_end - start) {
    return 0;
  }
  next_ = start + static_cast<std::uint32_t>(size);
  return start;
}

HostMemory::CopyTarget HostMemory::copy_target(const void* pointer, std::size_t size) {
  if (!pointer) {
    return {};
  }
  auto& storage = copy_targets_[pointer];
  CopyTarget target;
  if (storage.size < size) {
    ++epoch_;
    if (storage.size) {
      target.replaced = storage.address;
      target.replaced_size = storage.size;
      copy_addresses_.erase(storage.address);
    }
    const auto address = reserve(size);
    if (!address) {
      return {};
    }
    storage.address = address;
    storage.size = size;
    storage.bytes = std::make_unique<std::uint8_t[]>(size + 32);
    copy_addresses_[address] = &storage;
  }
  target.address = storage.address;
  target.size = storage.size;
  target.storage = storage.data();
  return target;
}

void HostMemory::set_array(std::uint32_t address, std::size_t size, bool host_order) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto* data = pointer(address, size);
  auto& array = arrays_[address];
  if (array.data && !array.host_order && (array.data != data || array.size != size || host_order)) {
    arrays_by_pointer_.erase(array.data);
  }
  const bool fresh = array.data != data || array.size != size || array.host_order != host_order;
  if (fresh && array.data) {
    ++epoch_; // A description others may have cached changes.
  }
  array.size = size;
  array.host_order = host_order;
  array.data = data;
  if (fresh) {
    array.feed = 0;
  }
  if (host_order || !data) {
    if (fresh || !data) {
      array.generation = 0;
    }
  } else if (fresh) {
    array.generation = ++writes_;
    arrays_by_pointer_[data] = address;
    largest_array_ = std::max(largest_array_, size);
  }
}

void HostMemory::note_texture(const void* pointer, std::size_t size, std::uint32_t version) {
  const auto* p = static_cast<const std::uint8_t*>(pointer);
  if (!p || !size || (pool_ && p >= pool_ && p < pool_ + pool_size_)) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto& texture = textures_[static_cast<const std::uint8_t*>(pointer)];
  if (!texture.generation || texture.version != version || texture.size < size) {
    texture.size = std::max(texture.size, size);
    texture.version = version;
    texture.generation = ++writes_;
    largest_texture_ = std::max(largest_texture_, texture.size);
  }
}

std::uint64_t HostMemory::source_generation(const void* pointer) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = textures_.find(static_cast<const std::uint8_t*>(pointer));
  return found == textures_.end() ? 0 : found->second.generation;
}

void HostMemory::note_write(const void* pointer, std::size_t size) {
  const auto* begin = static_cast<const std::uint8_t*>(pointer);
  if (begin && size && begin < arena_ + arena_size_ && begin + size > arena_) {
    const auto first = static_cast<std::size_t>(std::max(begin, arena_) - arena_) >> block_bits;
    const auto last = static_cast<std::size_t>(std::min(begin + size, arena_ + arena_size_) - arena_ - 1) >> block_bits;
    const auto stamp = block_clock_.fetch_add(1, std::memory_order_relaxed) + 1;
    for (auto b = first; b <= last; ++b) {
      block_stamps_[b].store(stamp, std::memory_order_relaxed);
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!begin || !size) {
    return;
  }
  if (!textures_.empty()) {
    const auto generation = ++writes_;
    auto it = textures_.lower_bound(begin + size);
    while (it != textures_.begin()) {
      --it;
      if (it->first + largest_texture_ <= begin) {
        break;
      }
      if (it->first + it->second.size > begin) {
        it->second.generation = generation;
      }
    }
  }
  if (arrays_by_pointer_.empty()) {
    return;
  }
  const auto generation = ++writes_;
  auto it = arrays_by_pointer_.lower_bound(begin + size);
  while (it != arrays_by_pointer_.begin()) {
    --it;
    auto found = arrays_.find(it->second);
    if (it->first + largest_array_ <= begin) {
      break;
    }
    if (found != arrays_.end() && it->first + found->second.size > begin) {
      found->second.generation = generation;
    }
  }
}

std::uint8_t* HostMemory::pointer(std::uint32_t address, std::size_t size) const {
  if (address >= window_base && !copy_addresses_.empty()) {
    auto copy = copy_addresses_.upper_bound(address);
    if (copy != copy_addresses_.begin()) {
      --copy;
      const std::size_t offset = address - copy->first;
      if (offset < copy->second->size) {
        return size <= copy->second->size - offset ? copy->second->data() + offset : nullptr;
      }
    }
  }
  if (address >= arena_bias_ && address < window_base) {
    const std::size_t offset = address - arena_bias_;
    if (offset > arena_size_ || size > arena_size_ - offset) {
      return nullptr;
    }
    return const_cast<std::uint8_t*>(arena_ + offset);
  }
  auto it = window_.upper_bound(address);
  if (it == window_.begin()) {
    return nullptr;
  }
  --it;
  const std::size_t offset = address - it->first;
  if (offset > it->second.size || size > it->second.size - offset) {
    return nullptr;
  }
  return const_cast<std::uint8_t*>(it->second.data + offset);
}

void HostMemory::reset_window() {
  ++epoch_;
  window_.clear();
  named_.clear();
  next_ = window_base;
  if (region_) {
    window_[region_address_] = {region_, region_size_};
    next_ = region_address_ + static_cast<std::uint32_t>(region_size_);
  }
  for (auto it = arrays_.begin(); it != arrays_.end();) {
    it = it->first >= window_base ? arrays_.erase(it) : std::next(it);
  }
}

bool HostMemory::copy(std::uint32_t address, std::uint8_t* destination, std::size_t size) {
  const auto* source = pointer(address, size);
  if (!source) {
    return false;
  }
  std::memcpy(destination, source, size);
  return true;
}

starlet::gx::ByteView HostMemory::view(std::uint32_t address, std::size_t size) {
  const auto* source = pointer(address, size);
  return source ? starlet::gx::ByteView{source, size} : starlet::gx::ByteView{};
}

starlet::gx::MemoryArrayView HostMemory::array_view(std::uint32_t address) {
  const auto found = arrays_.find(address);
  if (found == arrays_.end()) {
    return {};
  }
  const auto* data = pointer(address, found->second.size);
  if (!data) {
    return {};
  }
  auto& array = found->second;
  if (array.host_order && array.feed != feed_ && array.size <= max_digest_bytes) {
    const auto digest = XXH3_64bits(data, array.size);
    if (digest != array.digest || !array.generation) {
      std::lock_guard<std::mutex> lock(mutex_);
      array.digest = digest;
      array.generation = ++writes_;
    }
    array.feed = feed_;
  }
  starlet::gx::MemoryArrayView view;
  view.bytes = {data, array.size};
  view.host_order = array.host_order;
  view.generation = array.host_order && array.feed != feed_ ? 0 : array.generation;
  return view;
}

} // namespace aurora::starlet_backend
