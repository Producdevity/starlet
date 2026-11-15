#pragma once

// The pieces of AX's audio diagnostics that do not depend on the DSP model: voice parameter checks,
// the rate limiter, the rolling clip buffer and the WAV writer. diagnostics.cpp adds the always-on
// state ax_sdk.cpp feeds once per 5 ms frame.

#include "starlet/ax/diagnostics.hpp"
#include "starlet/sdk/ax.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace starlet::ax::diag {

using namespace starlet::sdk;

// What check_voice finds in a running voice's parameter block.
enum Anomaly : std::uint32_t {
    ratio_zero = 1u << 0,        // the SRC ratio is 0: the voice never advances
    ratio_high = 1u << 1,        // above 2.0: more than an octave up
    src_polyphase = 1u << 2,     // srcSelect 0, the 4-tap converter
    src_invalid = 1u << 3,       // srcSelect above 2
    address_range = 1u << 4,     // loop, end or current address outside the sample memory
    adpcm_predictor = 1u << 5,   // predictor/scale word above 0x7F
    adpcm_scale = 1u << 6,       // scale above 12
    loop_context = 1u << 7,      // a looped ADPCM voice with an all-zero loop context
    lpf_on = 1u << 8,
    itd_on = 1u << 9,
};
constexpr std::uint32_t severe_mask = ratio_zero | ratio_high | src_invalid | address_range | adpcm_predictor | adpcm_scale |
                                      loop_context;

std::uint32_t check_voice(const AXPB& pb, std::size_t memory_bytes);
// "ratio-high lpf-on", or "-" for none.
std::string flag_names(std::uint32_t flags);
double voice_ratio(const AXPB& pb);
// One line (no newline): voice, format, ratio, loop flag and addresses.
std::string describe_voice(int index, const AXPB& pb);

// At most `budget` lines per window of `window_frames` frames. allow() returns false for the rest and
// counts them; the first allow() of a new window reports how many the last one dropped.
class RateLimiter {
public:
    RateLimiter(unsigned budget, std::uint64_t window_frames) : budget_(budget), window_(window_frames) {}
    bool allow(std::uint64_t frame, unsigned* suppressed_before = nullptr);
    // Lines dropped in the current window.
    unsigned suppressed() const { return suppressed_; }

private:
    unsigned budget_;
    std::uint64_t window_;
    std::uint64_t window_start_ = 0;
    unsigned used_ = 0, suppressed_ = 0;
};

constexpr unsigned clip_channels = 4;
constexpr unsigned clip_rate = 32000;

// The last `capacity` frames of four channels, interleaved: main L, main R, aux A, aux B.
class ClipRing {
public:
    explicit ClipRing(std::size_t capacity_frames);
    void push(const s16* main_l, const s16* main_r, const s16* aux_a, const s16* aux_b, std::size_t count);
    std::size_t size() const { return filled_; }
    std::size_t capacity() const { return capacity_; }
    // Frames pushed in all; the oldest kept frame is total() - size().
    std::uint64_t total() const { return total_; }
    // The kept frames, oldest first, interleaved.
    std::vector<s16> snapshot() const;

private:
    std::size_t capacity_, head_ = 0, filled_ = 0;
    std::uint64_t total_ = 0;
    std::vector<s16> data_;
};

// A 16-bit PCM WAV, written to path + ".tmp" and renamed. frames counts sample frames.
bool write_wav(const std::string& path, const s16* interleaved, std::size_t frames, unsigned channels, unsigned rate);

// --- the always-on state, fed by ax_sdk.cpp ---

void reset();
// Once per frame for each running voice after the sync; started when it was not running before.
void running_voice(int index, const AXPB& pb, bool started, std::size_t memory_bytes);
void stopped_voice(int index);
// Once per frame after the aux callbacks. main is the AI buffer (right channel first); aux_a and aux_b
// are the aux buffers (L, R, S of 160 samples each) or null without a callback.
void end_frame(const s16* main_rl, const s32* aux_a, const s32* aux_b, unsigned main_saturated,
               unsigned reads_past_end);

}
