#pragma once

#include <cstdint>

namespace starlet::ax {

// Always-on audio diagnostics. AX writes "[audio]" lines to stderr and keeps the last 10 s of its
// output in a ring: the final 32 kHz stereo mix and the returns of aux bus A and B (each one channel,
// the mean of L and R). save_audio_clip writes the ring as a 4-channel 16-bit WAV (main L, main R,
// aux A return, aux B return) and a text file with the parameter blocks of the voices that started
// in that window.

// Seconds since the process started (the clock every "[audio]" line uses).
double diagnostics_seconds() noexcept;

// The folder the clips go to; empty keeps them off. A trailing slash is optional.
void set_diagnostics_directory(const char* directory);

// Writes audio-clip-<seconds>.wav and .txt, or audio-clip-exit.* when reason is "exit" (overwritten on
// every run). Blocks until the files are written; returns false when nothing was written.
bool save_audio_clip(const char* reason);

// The same on a worker thread, for a button handler. A request while one runs is dropped.
void request_audio_clip(const char* reason);

}
