#pragma once

#include <cstddef>
#include <cstdint>

// Draws Aurora's GX command stream with Starlet's OpenGL ES renderer instead of Aurora's
// own, when the environment sets AURORA_RENDERER=starlet.
//
// Two Aurora trees use this bridge, and each calls the part of it that fits:
//   upstream encounter/aurora: no Dawn, WebGPU or render worker. Aurora creates an OpenGL ES window
//     (set_gl_attributes, initialize), decodes the FIFO on the calling thread (process), and
//     end_frame submits the frame and presents it from a GL thread the bridge owns.
//   GLES fork bmdhacks/aurora: Aurora's render worker owns the context and calls present.
namespace aurora::starlet_backend {

bool enabled() noexcept;
// Main thread, from gx::fifo::drain: decodes and records one drained FIFO buffer.
void process(const std::uint8_t* data, std::size_t size);
// Main thread, after the frame's last drain: submits the frame's GL work. Upstream tree: also
// presents the frame (letterboxed 4:3 into the window) and swaps.
void end_frame();
// GLES fork, render worker, in the frame's end callback: draws the frame into the present target
// and leaves Aurora's GL state shadow invalid. False when nothing was drawn yet.
bool present(std::uint32_t target_framebuffer, std::uint32_t surface_width, std::uint32_t surface_height);
// Upstream tree, main thread, before the window is created: the SDL_GL attributes of an OpenGL ES
// 3.1 context. The window needs SDL_WINDOW_OPENGL as well.
void set_gl_attributes();
// Upstream tree, main thread, after the window exists: creates the context, starts the GL thread.
// False (and logged) when no context can be made.
bool initialize();
// Stops the GL work and releases the renderer and context; before the window is destroyed.
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
