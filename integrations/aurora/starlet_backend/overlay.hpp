#pragma once

union SDL_Event;
struct aurora_starlet_option;

// Optional performance overlay and settings menu (sbc-port-overlay). Without STARLET_WITH_OVERLAY
// every call does nothing. The overlay draws on the GL thread; events arrive on the main thread.
namespace aurora::starlet_backend::overlay {

// GL thread, after the frame is drawn into the window and before the swap.
void frame(int width, int height);
// Main thread: feeds one SDL event to the overlay. True when the overlay used it and the game
// must not see it.
bool event(const SDL_Event& event);
// GL thread, before the context is released.
void shutdown();
// Any thread: queues an option for the overlay's settings menu; see aurora_starlet_overlay_option.
bool add_option(const struct ::aurora_starlet_option& option);

} // namespace aurora::starlet_backend::overlay
