#pragma once

#include "aurora_commands.hpp"

#include <memory>

// RmlUi under AURORA_RENDERER=starlet, upstream tree with AURORA_ENABLE_RMLUI. RmlUi renders on
// the main thread into a recording; the GL thread draws the recording into the window's
// framebuffer after the game frame, in the same render pass, before the overlay and the swap.
//
// Layers are not rendered offscreen. Compositing a layer with opacity or brightness scales the
// colors of the layer's draws; blur, drop-shadow and Aurora's glass filter draw the layer
// unfiltered. A layer saved as a texture (box-shadow) renders once into its texture, before the
// window's pass. Clip masks are not supported, so clipping is rectangular.
namespace aurora::starlet_backend::rmlui {

struct Frame;

#if STARLET_AURORA_UPSTREAM && defined(AURORA_ENABLE_RMLUI)
// Main thread, when a frame is handed to the GL thread: the UI recorded since the last call, or
// null when nothing was recorded.
std::shared_ptr<Frame> take_frame();
// Main thread: true when a recorded frame waits that has something to draw.
bool frame_pending();
// GL thread, before the window's framebuffer is bound: renders layers saved as textures.
void prepare(Frame& frame);
// GL thread, after the game frame is drawn into the default framebuffer. Releases the GL objects
// of geometry and textures that RmlUi released before the frame was taken.
void draw(Frame& frame, int surface_width, int surface_height);
// GL thread, before the context is released.
void shutdown_gl();
#else
inline std::shared_ptr<Frame> take_frame() { return nullptr; }
inline bool frame_pending() { return false; }
inline void prepare(Frame&) {}
inline void draw(Frame&, int, int) {}
inline void shutdown_gl() {}
#endif

} // namespace aurora::starlet_backend::rmlui
