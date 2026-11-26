#pragma once

#include <cstddef>
#include <cstdint>

union SDL_Event;
namespace Rml {
class Context;
class RenderInterface;
} // namespace Rml

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
// Upstream tree, main thread, from Aurora's SDL event loop: offers an event to the optional
// performance overlay. True when the overlay used it, so the game must not see it.
bool overlay_event(const union SDL_Event& event);
// Upstream tree with AURORA_ENABLE_RMLUI: the render interface RmlUi draws through, or null when
// the bridge is off.
Rml::RenderInterface* rmlui_interface();
// Main thread, before end_frame: records what the context shows, drawn over the game frame into
// the window rectangle (x, y from the top left, in pixels) at the next present.
void rmlui_record(Rml::Context& context, int x, int y, int width, int height);
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

extern "C" {
// The game's DCStoreRange and DCFlushRange stubs call this.
void aurora_starlet_note_write(const void* pointer, unsigned size);

// Opt-in. Game thread, after aurora_begin_frame and before the frame's first draw: the frame's GX
// state updates and its draws are recorded, but the GL thread does not render them and the frame
// is not presented. A frame that copies or reads the EFB, or fills a segment, renders anyway,
// since later frames may sample the copy; it is still not presented. Without it, and without
// AURORA_RENDERER=starlet, nothing changes.
void aurora_starlet_skip_frame(void);

enum { AURORA_STARLET_OPTION_TOGGLE = 0, AURORA_STARLET_OPTION_CHOICE = 1 };
// One row of the overlay menu. Strings are copied. A page is created by its first option, and
// all options of a page must be added together. Add them before the first presented frame; the
// overlay refuses later ones. key and page_key use a-z, 0-9, _ and -, at most 32 characters.
typedef struct aurora_starlet_option {
  const char* page_key;
  const char* page_label;
  int type;
  const char* key;
  const char* label;
  const char* help;
  int default_value; // A toggle's 0 or 1, or a choice's index; a saved value replaces it.
  const char* const* choices;
  unsigned choice_count;
  // Runs when the option registers (with the saved or default value) and on every change, on the
  // GL thread or the main thread, with the overlay locked. Return 0 to accept the value.
  int (*on_change)(int value, void* user);
  void* user;
} aurora_starlet_option;
// Any thread. False when the overlay is not built in.
int aurora_starlet_overlay_option(const aurora_starlet_option* option);
}
