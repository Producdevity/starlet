#pragma once

#include <cstdint>
#include <string_view>

namespace starlet::gx {
class State;
}

namespace aurora::starlet_backend::issues {

// Problems that can change the picture, logged as "[starlet-issue] HH:MM:SS frame N: kind" so a
// screenshot's time leads to its cause. Each kind logs when it first happens, with the GX state
// of the draw involved when there is one, and again whenever it returns after a quiet stretch.
enum class Occurrence { quiet, first, returned };
// Counts one occurrence and says whether to log it; frequent sites format their detail only then:
//   if (auto when = issues::note(kind); when != issues::Occurrence::quiet) issues::write(kind, when, ...);
Occurrence note(std::string_view kind);
void write(std::string_view kind, Occurrence when, const starlet::gx::State* state = nullptr, std::string_view detail = {});
inline void report(std::string_view kind, const starlet::gx::State* state = nullptr, std::string_view detail = {}) {
  if (const auto when = note(kind); when != Occurrence::quiet) {
    write(kind, when, state, detail);
  }
}
void set_frame(std::uint64_t frame);

} // namespace aurora::starlet_backend::issues
