#pragma once

#include <cstdint>

namespace starlet::gx_fifo {

// Development: timestamps of the steps between a frame's last draw and its presentation, one
// "microseconds kind value" line per event. Kinds:
//   r retrace timer fired      R retrace handled by the guest
//   d GXSetDrawDone            D draw done reached the GPU thread
//   e GXWaitDrawDone returned  S VISetNextFrameBuffer
//   F VIFlush                  P present on the GPU thread
//   w VIWaitForRetrace returned
//   h, g, x guest CPU handed to a thread, resumed, let go (starlet/os/guest_cpu.hpp set_switch_trace)
// Ports add their own (for example W for the swap on their GL thread).

// Starts recording; the last 262144 events are kept. Call before the game starts.
void start_frame_trace();
// Any thread; does nothing until start_frame_trace.
void frame_event(char kind, std::uint32_t value = 0);
// Writes the events recorded so far; false when tracing is off or path cannot be written.
bool write_frame_trace(const char* path);

}
