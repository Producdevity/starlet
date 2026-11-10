#pragma once

#include <chrono>
#include <functional>

union SDL_Event;

namespace starlet::host {

// The frontend asks a port to leave with SIGTERM; gptokeyb's exit chord sends it, waits
// 3 s, then sends SIGKILL. A port flushes its saves and exits within 1 s of the request:
// it checks quit_requested() where its main loop turns (or waits with wait_for_quit) and
// calls exit_now(), which runs the save flushes registered with on_quit. exit_after_quit
// backs that path up when the loop can stall.

// Installs SIGTERM and SIGINT handlers that only request the quit. Once per process; before
// or after SDL_Init (SDL keeps a handler it did not install).
void install_quit_signals();

// Async-signal-safe: sets the flag and wakes every wait_for_quit.
void request_quit() noexcept;
bool quit_requested() noexcept;
// The signal that requested the quit, or 0 when code did.
int quit_signal() noexcept;
// True once the quit is requested, false when timeout passes first.
bool wait_for_quit(std::chrono::milliseconds timeout);
void wait_for_quit();

// Polls SDL's event queue: SDL_QUIT requests the quit, and each event goes to handler when
// one is given. On the thread that owns SDL's events.
void poll_events(const std::function<void(const SDL_Event&)>& handler = nullptr);

// A save flush for exit_now, which runs them in registration order. It may run on whichever
// thread exits, so it locks what it writes and waits for no thread that can itself call
// exit_now.
void on_quit(std::function<void()> flush);
// Runs the flushes once, flushes stdio and ends the process with status. A second caller
// waits while the first one exits.
[[noreturn]] void exit_now(int status);
// Starts a thread that, grace after a quit request, calls exit_now(0): the exit the main
// loop missed because it was stalled (loading, a guest wait, a shader build).
void exit_after_quit(std::chrono::milliseconds grace);

}
