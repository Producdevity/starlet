// AX against hand-built ARAM and parameter blocks: the accelerator's decoding and looping, sample-rate
// conversion, the mixer, aux routing, the compressor, voice allocation and the AI-paced frames.
#include "../ai/fake_ai_platform.hpp"
#include "../../src/ax/ax_internal.hpp"

#include "starlet/ai/ai.hpp"
#include "starlet/ax/ax.hpp"
#include "starlet/sdk/ai.h"
#include "starlet/sdk/ax.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace starlet::sdk;
namespace detail = starlet::ax::detail;

namespace fake_ax {
extern std::vector<std::uint8_t> aram;
}

// The compressor table the port links from the SDK: here frame f has the constant gain 0x4000 - f * 0x200.
u16 __AXCompressorTable[3360];

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
template <typename T>
void check_eq(T actual, T expected, const char* what) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (got %lld, expected %lld)\n", what, (long long)actual, (long long)expected);
        ++failures;
    }
}

const detail::Memory memory() { return {fake_ax::aram.data(), fake_ax::aram.size()}; }

void put16(u32 word, s16 value) {
    fake_ax::aram[2 * word] = std::uint8_t(u16(value) >> 8);
    fake_ax::aram[2 * word + 1] = std::uint8_t(value);
}

void set_addr(AXPB& pb, u16 format, u16 loop, u32 loop_address, u32 end, u32 current) {
    pb.addr = AXPBADDR{loop, format, u16(loop_address >> 16), u16(loop_address), u16(end >> 16), u16(end),
                       u16(current >> 16), u16(current)};
}

u32 current(const AXPB& pb) { return (u32(pb.addr.currentAddressHi) << 16) | pb.addr.currentAddressLo; }

std::vector<s16> read(AXPB& pb, u32 count) {
    std::vector<s16> out(count);
    detail::read_samples(pb, memory(), out.data(), count);
    return out;
}

AXPB pcm16_block(u32 loop_address, u32 end, u32 start, bool loop) {
    AXPB pb{};
    pb.state = AX_PB_STATE_RUN;
    pb.srcSelect = 2;
    set_addr(pb, AX_PB_FORMAT_PCM16, loop, loop_address, end, start);
    pb.adpcm.gain = 0x800;
    pb.src.ratioHi = 1;
    return pb;
}

// One 8-byte ADPCM frame: header 0x12 (coefficient pair 1, scale 4), nibbles 1, 2, -1, 7, -8, then
// zeros; the next frame: header 0x00, nibble 3. With pair 1 = (0x400, 0) and empty history each
// sample is scale * nibble + ((0x400 + 0x400 * previous) >> 11):
//   4, 8 + (5120 >> 11) = 10, -4 + (11264 >> 11) = 1, 28 + (2048 >> 11) = 29, -32 + (30720 >> 11) = -17,
//   (-16384 >> 11) = -8, -4, -2, -1, 0, 0, 0, 0, 0; then 3 + (0x400 >> 11) = 3 with pair 0 = (0, 0).
void adpcm_decoding() {
    const std::uint8_t frames[16] = {0x12, 0x12, 0xF7, 0x80, 0, 0, 0, 0, 0x00, 0x30, 0, 0, 0, 0, 0, 0};
    std::memcpy(&fake_ax::aram[0x100], frames, sizeof frames);
    AXPB pb{};
    pb.state = AX_PB_STATE_RUN;
    pb.srcSelect = 2;
    set_addr(pb, AX_PB_FORMAT_ADPCM, 0, 0x202, 0x2FF, 0x202);
    pb.adpcm.a[1][0] = 0x400;
    pb.adpcm.pred_scale = 0x12;
    const auto out = read(pb, 15);
    const s16 expected[15] = {4, 10, 1, 29, -17, -8, -4, -2, -1, 0, 0, 0, 0, 0, 3};
    check(std::equal(out.begin(), out.end(), expected), "ADPCM frame decodes as computed by hand");
    check_eq<u32>(current(pb), 0x213, "the decoder skips the next frame's header");
    check_eq<u16>(pb.adpcm.pred_scale, 0x00, "the next frame's header becomes the predictor and scale");
    check_eq<s16>(s16(pb.adpcm.yn1), 3, "history: last sample");
    check_eq<s16>(s16(pb.adpcm.yn2), 0, "history: the one before");
}

void looping() {
    for (u32 i = 0; i < 8; ++i) put16(0x1000 + i, s16(100 + i));
    AXPB pb = pcm16_block(0x1002, 0x1005, 0x1000, true);
    auto out = read(pb, 10);
    const s16 looped[10] = {100, 101, 102, 103, 104, 105, 102, 103, 104, 105};
    check(std::equal(out.begin(), out.end(), looped), "a looping voice wraps from its end to its loop address");
    check_eq<u16>(pb.state, AX_PB_STATE_RUN, "a looping voice keeps running");

    pb = pcm16_block(0x1002, 0x1005, 0x1000, false);
    out = read(pb, 10);
    const s16 once[10] = {100, 101, 102, 103, 104, 105, 0, 0, 0, 0};
    check(std::equal(out.begin(), out.end(), once), "a one-shot voice stops after its end address");
    check_eq<u16>(pb.state, AX_PB_STATE_STOP, "a one-shot voice stops");

    // ADPCM loops restore the loop context; streams keep their history.
    fake_ax::aram[0x200] = 0x10;
    std::memset(&fake_ax::aram[0x201], 0, 7);
    for (u16 type : {AX_PB_TYPE_NORMAL, AX_PB_TYPE_STREAM}) {
        AXPB adpcm{};
        adpcm.state = AX_PB_STATE_RUN;
        adpcm.type = type;
        adpcm.srcSelect = 2;
        set_addr(adpcm, AX_PB_FORMAT_ADPCM, 1, 0x402, 0x403, 0x402);
        adpcm.adpcm.a[1][0] = 0x800;
        adpcm.adpcm.pred_scale = 0x10;
        adpcm.adpcm.yn1 = 100;
        adpcm.adpcmLoop = AXPBADPCMLOOP{0x10, 500, 0};
        out = read(adpcm, 4);
        if (type == AX_PB_TYPE_NORMAL)
            check(out[0] == 100 && out[1] == 100 && out[2] == 500 && out[3] == 500, "an ADPCM loop restarts from the loop context");
        else
            check(out[0] == 100 && out[1] == 100 && out[2] == 100 && out[3] == 100, "an ADPCM stream keeps its history over the loop");
    }
}

void sample_rate_conversion() {
    for (u32 i = 0; i < 400; ++i) put16(0x2000 + i, s16(i * 10));
    for (u16 select : {u16(0), u16(1)}) {
        AXPB pb = pcm16_block(0x2000, 0x3000, 0x2000, false);
        pb.srcSelect = select;
        pb.src.ratioHi = 1;
        read(pb, 160);
        check_eq<u32>(current(pb) - 0x2000, 160, select ? "linear SRC at 1.0 reads 160 samples a frame" : "4-tap SRC at 1.0 reads 160 samples a frame");
        pb = pcm16_block(0x2000, 0x3000, 0x2000, false);
        pb.srcSelect = select;
        pb.src.ratioHi = 0;
        pb.src.ratioLo = 0x8000;
        const auto out = read(pb, 160);
        check_eq<u32>(current(pb) - 0x2000, 80, select ? "linear SRC at 0.5 reads 80 samples a frame" : "4-tap SRC at 0.5 reads 80 samples a frame");
        if (select == 1) {
            // Interpolates between the oldest two of the four history samples: halfway steps of 5.
            check(out[10] - out[9] == 5 && out[11] - out[10] == 5, "linear SRC at 0.5 interpolates halfway");
        }
    }
    const s16* coefs = detail::src_coefficients();
    for (int table = 0; table < 3; ++table) {
        int peak = 0;
        for (int phase = 0; phase < 128; ++phase) {
            const s16* c = coefs + table * 0x200 + phase * 4;
            peak = std::max(peak, c[0] + c[1] + c[2] + c[3]);
        }
        check(peak >= 32765 && peak <= 32769, "4-tap tables have unity peak gain");
    }
    check(std::all_of(coefs + 0x600, coefs + 0x800, [](s16 c) { return c == 0; }), "the fourth table is empty");
}

// --- Through AXInit, the AI DMA and the frame interrupts ---

std::vector<s16> play(int blocks) { return fake_ai::pull(std::size_t(blocks) * 160); }

// The first 160-frame block of out that holds sound, or -1.
int first_sound(const std::vector<s16>& out) {
    for (std::size_t i = 0; i < out.size(); ++i)
        if (out[i]) return int(i / 320);
    return -1;
}

void restart() {
    AXQuit();
    AIInit(nullptr);
    AXInitEx(1);
}

const u32 constant_start = 0x4000;

AXVPB* constant_voice(s16 value, const AXPBMIX& mix, u32 priority = 15, AXVoiceCallback drop = nullptr) {
    for (u32 i = 0; i < 256; ++i) put16(constant_start + i, value);
    AXVPB* voice = AXAcquireVoice(priority, drop, 0);
    AXPBADDR addr{1, AX_PB_FORMAT_PCM16, 0, u16(constant_start), 0, u16(constant_start + 255), 0, u16(constant_start)};
    AXSetVoiceAddr(voice, &addr);
    AXSetVoiceSrcType(voice, AX_SRC_TYPE_NONE);
    AXPBVE ve{0x7FFF, 0};
    AXSetVoiceVe(voice, &ve);
    AXPBMIX m = mix;
    AXSetVoiceMix(voice, &m);
    AXSetVoiceState(voice, AX_PB_STATE_RUN);
    return voice;
}

// 0x4000 through the envelope at 0x7FFF is 16383; L ramps from 0 by 0x100 a sample, and keeps its
// volume into the next frame.
void volume_ramps() {
    restart();
    AXPBMIX mix{};
    mix.vDeltaL = 0x100;
    constant_voice(0x4000, mix);
    const auto out = play(8);
    const int block = first_sound(out);
    check(block >= 0, "the voice plays");
    if (block < 0) return;
    bool ramp = true, right_silent = true;
    for (int i = 0; i < 256; ++i) {
        const int expected = (16383 * (0x100 * i)) >> 15;
        ramp &= out[(block * 160 + i) * 2] == expected;
        right_silent &= out[(block * 160 + i) * 2 + 1] == 0;
    }
    check(ramp, "the main left volume ramps per sample across frames");
    check(right_silent, "a voice mixed to the left only leaves the right silent");
}

int aux_calls = 0;
s32 aux_seen = 0;
void aux_callback(void* data, void*) {
    ++aux_calls;
    const auto* buffers = static_cast<AX_AUX_DATA*>(data);
    aux_seen = std::max(aux_seen, buffers->l[0]);
}

void aux_routing() {
    restart();
    AXPBMIX mix{};
    mix.vAuxAL = 0x8000;
    constant_voice(0x4000, mix);
    auto out = play(10);
    check(first_sound(out) < 0, "aux A without a callback returns nothing");

    AXRegisterAuxACallback(aux_callback, nullptr);
    aux_calls = 0;
    out = play(10);
    check(aux_calls == 10, "the aux A callback runs once a frame");
    check_eq<s32>(aux_seen, 16383, "the aux A callback gets the voice's send");
    const int block = first_sound(out);
    check(block >= 0 && out[(block * 160 + 5) * 2] == 16383 && out[(block * 160 + 5) * 2 + 1] == 0,
          "the aux A return reaches main left only");
    AXRegisterAuxACallback(nullptr, nullptr);
}

// Dolby Pro Logic II: aux B L/R carry the rear channels, which loop back into main L/R a frame later.
void dpl2() {
    restart();
    AXSetMode(AX_MODE_DPL2);
    AXPBMIX mix{};
    mix.vAuxBR = 0x8000;
    constant_voice(0x4000, mix);
    const auto out = play(10);
    const int block = first_sound(out);
    check(block >= 0 && out[(block * 160 + 5) * 2] == 0 && out[(block * 160 + 5) * 2 + 1] == 16383,
          "a DPL2 rear-right send reaches main right");
    AXSetMode(AX_MODE_STEREO);
}

void compressor() {
    for (int f = 0; f < 21; ++f)
        for (int i = 0; i < 160; ++i) __AXCompressorTable[f * 160 + i] = u16(0x4000 - f * 0x200);
    // Each voice's contribution saturates at 16 bits; two at 24575 sum to 49150, over the threshold.
    restart();
    AXPBMIX mix{};
    mix.vL = 0x8000;
    constant_voice(0x6000, mix);
    constant_voice(0x6000, mix);
    auto out = play(8);
    const int block = first_sound(out);
    check(block >= 0 && block + 1 < 8, "the loud voice plays");
    if (block < 0 || block + 1 >= 8) return;
    check_eq<s16>(out[block * 320], s16((49150 * 0x4000) >> 15), "the first compressed frame uses the first attack gains");
    check_eq<s16>(out[(block + 1) * 320], s16((49150 * (0x4000 - 10 * 0x200)) >> 15), "a held trigger uses the last attack frame");
    restart();
    AXSetCompressor(0);
    constant_voice(0x6000, mix);
    constant_voice(0x6000, mix);
    out = play(8);
    const int plain = first_sound(out);
    // The first frame's command list was built at AXInit, with the compressor on.
    check(plain >= 0 && plain + 1 < 8 && out[(plain + 1) * 320 + 2] == 32767, "without the compressor the output clips");
}

AXVPB* dropped = nullptr;
int drops = 0;
void drop_callback(void* voice) {
    dropped = static_cast<AXVPB*>(voice);
    ++drops;
}

void priorities() {
    restart();
    AXVPB* first = nullptr;
    AXVPB* second = nullptr;
    for (u32 i = 0; i < AX_MAX_VOICES; ++i) {
        AXVPB* v = AXAcquireVoice(5, drop_callback, i);
        check(v != nullptr, "64 voices are free");
        if (i == 0) first = v;
        if (i == 1) second = v;
    }
    drops = 0;
    check(AXAcquireVoice(5, drop_callback, 0) == nullptr, "no voice of lower priority to drop");
    check(drops == 0, "a refused acquire drops nothing");
    AXVPB* taken = AXAcquireVoice(10, drop_callback, 77);
    check(taken == first && dropped == first && drops == 1, "a higher priority drops the oldest lowest-priority voice");
    check(taken->priority == 10 && taken->userContext == 77, "the dropped voice is acquired anew");
    AXSetVoicePriority(second, 3);
    check(AXAcquireVoice(4, drop_callback, 0) == second && dropped == second, "a voice moved to a lower priority drops first");
    AXFreeVoice(taken);
    check(taken->priority == 0, "a freed voice has priority 0");

    // Over the SDK's DSP budget, a running voice is dropped from the frame interrupt.
    restart();
    drops = 0;
    AXPBMIX mix{};
    mix.vL = 0x8000;
    AXVPB* voice = constant_voice(0x1000, mix, 15, drop_callback);
    AXSetMaxDspCycles(1);
    play(4);
    check(drops == 1 && dropped == voice && voice->priority == 0, "a voice over the DSP budget is dropped and freed");
    check(starlet::ax::stats().dropped_voices >= 1, "stats count budget drops");
}

int frame_callbacks = 0;
void frame_callback() { ++frame_callbacks; }

void frame_cadence() {
    restart();
    AXRegisterCallback(frame_callback);
    frame_callbacks = 0;
    const auto before = starlet::ai::stats().completions;
    play(10);
    check_eq<int>(frame_callbacks, 10, "one frame callback per AI block");
    check_eq<std::uint64_t>(starlet::ai::stats().completions - before, 10, "one AI completion per 160 frames");

    // A guest that does not take the interrupt: the DMA repeats, frames wait, then catch up.
    fake_ai::immediate = false;
    const auto repeats = starlet::ai::stats().repeats;
    play(3);
    check_eq<int>(frame_callbacks, 10, "no frame runs while the guest holds the interrupt");
    check(starlet::ai::stats().repeats > repeats, "the DMA replays its block meanwhile");
    fake_ai::run_interrupts();
    fake_ai::immediate = true;
    check_eq<int>(frame_callbacks, 13, "the queued interrupts each run a frame");
    AXRegisterCallback(nullptr);
}

void lpf_coefficients() {
    u16 a0 = 0, b0 = 0;
    AXGetLpfCoefs(16000, &a0, &b0);
    check(b0 >= 5621 && b0 <= 5623 && a0 == 0x7FFF - b0, "LPF coefficients at 16 kHz");
}

}

int main() {
    adpcm_decoding();
    looping();
    sample_rate_conversion();
    lpf_coefficients();
    volume_ramps();
    aux_routing();
    dpl2();
    compressor();
    priorities();
    frame_cadence();
    AXQuit();
    std::printf("%s\n", failures ? "ax: failures" : "ax: ok");
    return failures ? 1 : 0;
}
