#pragma once

#include <cstddef>
#include <cstdint>

// Draws Aurora's GX command stream with Starlet's OpenGL ES renderer instead of Aurora's
// own, when the environment sets AURORA_RENDERER=starlet. Aurora keeps its window, UI and
// presentation; Starlet's GL work runs on Aurora's render worker, which owns the context.
namespace aurora::starlet_backend {

bool enabled() noexcept;
// Main thread, from gx::fifo::drain: decodes and records one drained FIFO buffer.
void process(const std::uint8_t* data, std::size_t size);
// Main thread, after the frame's last drain: submits the frame's GL work.
void end_frame();
// Render worker, in the frame's end callback: draws the frame into the present target and
// leaves Aurora's GL state shadow invalid. False when nothing was drawn yet.
bool present(std::uint32_t target_framebuffer, std::uint32_t surface_width, std::uint32_t surface_height);
void shutdown();
// Any thread: the game stored, flushed or invalidated a range of its data cache, which it
// does around CPU writes and DMA into memory the GPU reads.
void note_write(const void* pointer, std::size_t size);
// Game thread, from GXLoadTexObj: the data pointer the command stream should carry. Memory
// rewritten during the frame, such as a font's glyph cache page, is copied, so each draw
// reads the texture as it was when loaded, as the console and Aurora's renderer do; the
// stream is decoded only when the frame's FIFO drains.
const void* texture_data(const void* data, std::size_t size);
// Game thread, from GXCallDisplayList before it copies the list into the FIFO: names the
// copy's source, so draws from an unchanged list are known without reading their bytes.
void note_display_list(const void* data, std::size_t size);

} // namespace aurora::starlet_backend

// The game's DCStoreRange and DCFlushRange stubs call this.
extern "C" void aurora_starlet_note_write(const void* pointer, unsigned size);
