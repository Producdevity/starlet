#include "issues.hpp"

#include "../logging.hpp"

#include "starlet/gx/state.hpp"

#include <atomic>
#include <ctime>
#include <map>
#include <mutex>
#include <string>

namespace aurora::starlet_backend::issues {
namespace {
Module Log("aurora::starlet");

// About five seconds of frames: a kind that keeps happening logs once, one that comes back
// later logs again with the new time.
constexpr std::uint64_t quiet_frames = 300;
constexpr unsigned lines_per_kind = 40;
constexpr unsigned lines_total = 400;

struct Kind {
  std::uint64_t count = 0;
  std::uint64_t last_frame = 0;
  unsigned lines = 0;
};

std::mutex mutex;
std::map<std::string, Kind, std::less<>> kinds;
unsigned lines = 0;
std::atomic<std::uint64_t> current_frame{0};

std::string wall_time() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  char text[16];
  std::strftime(text, sizeof text, "%H:%M:%S", &local);
  return text;
}

void dump(const starlet::gx::State& state) {
  for (unsigned row = 0; row < 0x100; row += 16) {
    std::string words;
    for (unsigned a = row; a < row + 16; ++a) words += fmt::format(" {:06x}", state.bp(static_cast<std::uint8_t>(a)));
    Log.warn("[starlet-issue]   bp {:02x}:{}", row, words);
  }
  for (unsigned row = 0x50; row < 0xc0; row += 16) {
    std::string words;
    for (unsigned a = row; a < row + 16; ++a) words += fmt::format(" {:08x}", state.cp(static_cast<std::uint8_t>(a)));
    Log.warn("[starlet-issue]   cp {:02x}:{}", row, words);
  }
  for (unsigned row = 0x1000; row < 0x1058; row += 8) {
    std::string words;
    for (unsigned a = row; a < row + 8; ++a) words += fmt::format(" {:08x}", state.xf(static_cast<std::uint16_t>(a)));
    Log.warn("[starlet-issue]   xf {:04x}:{}", row, words);
  }
}
} // namespace

Occurrence note(std::string_view kind) {
  std::lock_guard<std::mutex> lock(mutex);
  auto found = kinds.find(kind);
  if (found == kinds.end()) {
    found = kinds.emplace(std::string(kind), Kind{}).first;
  }
  Kind& entry = found->second;
  const std::uint64_t frame = current_frame.load(std::memory_order_relaxed);
  const bool first = entry.count++ == 0;
  const bool returned = !first && frame - entry.last_frame >= quiet_frames;
  entry.last_frame = frame;
  if (!(first || returned) || entry.lines >= lines_per_kind || lines >= lines_total) {
    return Occurrence::quiet;
  }
  ++entry.lines;
  ++lines;
  if (entry.lines == lines_per_kind) {
    Log.warn("[starlet-issue] '{}' is only counted after its next line", kind);
  } else if (lines == lines_total) {
    Log.warn("[starlet-issue] issue log full; issues after the next line are only counted");
  }
  return first ? Occurrence::first : Occurrence::returned;
}

void write(std::string_view kind, Occurrence when, const starlet::gx::State* state, std::string_view detail) {
  std::uint64_t count = 0;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (const auto found = kinds.find(kind); found != kinds.end()) {
      count = found->second.count;
    }
  }
  Log.warn("[starlet-issue] {} frame {}: {}{}{}{}", wall_time(), current_frame.load(std::memory_order_relaxed), kind,
           detail.empty() ? "" : " (", detail, detail.empty() ? "" : ")");
  if (when == Occurrence::returned) {
    Log.warn("[starlet-issue]   back after a quiet stretch; {} so far", count);
  } else if (state) {
    dump(*state);
  }
}

void set_frame(std::uint64_t frame) { current_frame.store(frame, std::memory_order_relaxed); }

} // namespace aurora::starlet_backend::issues
