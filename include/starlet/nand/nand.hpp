#pragma once

// starlet::nand serves the Wii SDK's NAND calls (starlet/sdk/nand.h) from a save folder on the host.
// The port owns the folder; the game's calls bind to the C functions by name.

#include "starlet/sdk/nand.h"

namespace starlet::nand {

struct Hooks {
    // Runs an asynchronous call's completion as the console would: a port whose callbacks run as
    // interrupts brackets the call with its interrupt state. Without it the worker calls it directly.
    void (*complete)(sdk::NANDCallback callback, sdk::s32 result, sdk::NANDCommandBlock* block) = nullptr;
    // Bracket every section that holds the service's lock, for a port whose threads can be
    // cancelled (defer cancellation there). Both or neither.
    void (*enter)() = nullptr;
    void (*leave)() = nullptr;
};

// Opens `directory` (it must exist) and creates its data/ and tmp/ folders. Starts the worker for
// the asynchronous calls. Throws std::logic_error when already started, std::runtime_error when
// the folder cannot be used; on failure nothing stays open.
void start(const char* directory, const Hooks& hooks = {});
// Drains queued requests (their callbacks run), joins the worker and closes every handle. Call
// with no other thread inside a NAND call; never from a completion.
void stop() noexcept;

} // namespace starlet::nand
