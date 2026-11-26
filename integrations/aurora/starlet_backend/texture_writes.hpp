#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <unordered_set>

namespace aurora::starlet_backend {

// Pages written through the data-cache hooks this frame, and texture pointers seen rewritten
// while in use; both decide which texture loads are copied (texture_data). A copied texture is
// kept for later loads of the same pointer until a reported write reaches its bytes, so a
// texture the game does not rewrite is copied once instead of once per load.
class TextureWrites {
public:
  static constexpr unsigned page_bits = 12;
  // The first part holds one frame's copies; the rest holds the kept ones.
  static constexpr std::size_t frame_bytes = 4 << 20, kept_bytes = 12 << 20, pool_bytes = frame_bytes + kept_bytes;
  static constexpr unsigned verify_period = 16, max_refreshes = 4;

  TextureWrites() : pool_(static_cast<std::uint8_t*>(::operator new(pool_bytes, std::align_val_t(32)))) {}
  const std::uint8_t* pool() const { return pool_; }

  void note(const void* pointer, std::size_t size) {
    const auto first = reinterpret_cast<std::uintptr_t>(pointer);
    const auto begin = first >> page_bits;
    const auto end = (first + size - 1) >> page_bits;
    std::lock_guard<std::mutex> lock(mutex_);
    ++notes;
    for (auto page = begin; page <= end && page - begin < 4096; ++page) pages_.insert(page);
    if (kept_.empty()) return;
    auto it = kept_.lower_bound(first > largest_ ? first - largest_ : 0);
    for (; it != kept_.end() && it->first < first + size; ++it)
      if (it->first + it->second.size > first) it->second.stale = true;
  }

  // The bytes Starlet should read for a texture load of [data, data + size): the pointer itself, or a copy
  // of the bytes as they are now. copy_destination: data already is a copy Starlet owns.
  const void* load(const void* data, std::size_t size, bool copy_destination) {
    ++loads;
    if (!data || !size || copy_destination) return data;
    const auto at = reinterpret_cast<std::uintptr_t>(data);
    bool written = volatile_.count(data) != 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!written) for (auto page = at >> page_bits; page <= (at + size - 1) >> page_bits && !written; ++page) written = pages_.count(page);
    if (!written) return data;
    volatile_.insert(data);
    auto kept = kept_.find(at);
    if (kept != kept_.end() && kept->second.size == size && kept->second.refreshes <= max_refreshes) {
      auto& k = kept->second;
      const auto* bytes = pool_ + frame_bytes + k.offset;
      // A write the game did not report shows only in the bytes: compared on a staggered period.
      if (!k.stale && ((frames_ + k.phase) % verify_period != 0 || !std::memcmp(data, bytes, size))) {
        ++hits;
        return bytes;
      }
      k.stale = true;
    }
    const std::size_t kept_offset = (kept_used_ + 31) & ~std::size_t{31};
    const bool keep = kept == kept_.end() ? true : kept->second.refreshes < max_refreshes;
    if (keep && size <= kept_bytes - kept_offset) {
      std::memcpy(pool_ + frame_bytes + kept_offset, data, size);
      kept_used_ = kept_offset + size;
      largest_ = std::max(largest_, size);
      auto& k = kept_[at];
      k.refreshes = kept == kept_.end() ? 0 : k.refreshes + 1;
      k.offset = kept_offset; k.size = size; k.stale = false; k.phase = static_cast<std::uint8_t>((at >> 5) % verify_period);
      ++copies; copied_bytes += size;
      return pool_ + frame_bytes + kept_offset;
    }
    if (keep) reset_pending_ = true;
    const std::size_t offset = (used_ + 31) & ~std::size_t{31};
    if (size > frame_bytes - offset) {
      ++overflows;
      return data;
    }
    std::memcpy(pool_ + offset, data, size);
    used_ = offset + size;
    ++copies;
    copied_bytes += size;
    return pool_ + offset;
  }

  // Main thread, after the frame's FIFO drained: its copies have been decoded. True when the kept
  // copies were dropped to make room; the caller then reports their range as written.
  bool end_frame() {
    used_ = 0;
    ++frames_;
    std::lock_guard<std::mutex> lock(mutex_);
    pages_.clear();
    if (!reset_pending_) return false;
    reset_pending_ = false;
    kept_.clear();
    kept_used_ = 0;
    largest_ = 0;
    return true;
  }
  const std::uint8_t* kept_pool() const { return pool_ + frame_bytes; }

  std::uint64_t copies = 0, copied_bytes = 0, overflows = 0, notes = 0, loads = 0, hits = 0;

private:
  struct Kept {
    std::size_t offset = 0, size = 0;
    unsigned refreshes = 0;
    bool stale = false;
    std::uint8_t phase = 0;
  };
  std::uint8_t* pool_;
  std::size_t used_ = 0, kept_used_ = 0, largest_ = 0;
  std::uint64_t frames_ = 0;
  bool reset_pending_ = false;
  std::mutex mutex_;
  std::unordered_set<std::uintptr_t> pages_;
  std::unordered_set<const void*> volatile_;
  std::map<std::uintptr_t, Kept> kept_;
};

} // namespace aurora::starlet_backend
