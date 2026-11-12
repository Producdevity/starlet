// The AI DMA against hand-driven pulls: latching, interrupts, replay of a late block, registers.
#include "fake_ai_platform.hpp"

#include "starlet/ai/ai.hpp"
#include "starlet/sdk/ai.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace starlet::sdk;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

int interrupts = 0;
void dma_callback() { ++interrupts; }

void dma() {
    AIInit(nullptr);
    check(fake_ai::output_rate == 32028, "AI opens the output at 32028 Hz");
    check(AIGetDSPSampleRate() == AI_SAMPLERATE_32KHZ, "AI starts at the 32 kHz selector");
    AIRegisterDMACallback(dma_callback);
    // Blocks of 560 frames; samples are right channel first.
    alignas(32) static s16 a[1120], b[1120];
    for (unsigned i = 0; i < 560; ++i) {
        a[i * 2] = s16(i);
        a[i * 2 + 1] = s16(-int(i));
        b[i * 2] = s16(1000 + i);
        b[i * 2 + 1] = s16(-1000 - int(i));
    }
    starlet::ai::init_dma(a, sizeof a);
    AIStartDMA();
    check(AIGetDMAEnableFlag(), "the DMA runs");
    starlet::ai::init_dma(b, sizeof b);
    auto out = fake_ai::pull(500);
    check(out[0] == 0 && out[2] == -1 && out[3] == 1, "AI plays left then right");
    check(interrupts == 0, "no interrupt inside a block");
    check(AIGetDMABytesLeft() == 60 * 4, "bytes left in the playing block");
    out = fake_ai::pull(100);
    check(out[120] == -1000 && out[121] == 1000, "the programmed block follows");
    check(interrupts == 1, "one interrupt per finished block");
    // Nothing programmed in time: the block repeats after a bounded wait.
    out = fake_ai::pull(560);
    check(out[1040] == -1000 && out[1041] == 1000, "an unprogrammed DMA replays its block");
    const auto stats = starlet::ai::stats();
    check(stats.completions == 2 && stats.repeats == 1 && stats.frames == 1160, "completions, repeats and frames");
    check(stats.sample_rate == 32028 && stats.running, "stats show the running DMA");
    AIStopDMA();
    out = fake_ai::pull(10);
    check(std::all_of(out.begin(), out.end(), [](s16 v) { return v == 0; }), "a stopped DMA plays silence");
}

// Interrupts queue on the guest; the DMA keeps playing the latched blocks meanwhile.
void guest_addresses() {
    std::memset(fake_ai::memory.data(), 0, 64);
    fake_ai::memory[0] = 0x34;
    fake_ai::memory[1] = 0x12;
    AIInitDMA(0x80000000u, 32);
    check(AIGetDMAStartAddr() != 0 && AIGetDMALength() == 32, "AIInitDMA sets the registers");
    fake_ai::immediate = false;
    AIStartDMA();
    auto out = fake_ai::pull(8);
    check(out[1] == 0x1234, "a guest block plays (right channel)");
    check(fake_ai::queued_interrupts() == 1, "the finished block's interrupt waits for the guest");
    fake_ai::run_interrupts();
    AIStopDMA();
    fake_ai::immediate = true;
}

void sample_rate_and_stream() {
    AISetDSPSampleRate(AI_SAMPLERATE_48KHZ);
    check(fake_ai::output_rate == 48043 && AIGetDSPSampleRate() == AI_SAMPLERATE_48KHZ, "48 kHz reopens the output at 48043 Hz");
    AISetDSPSampleRate(AI_SAMPLERATE_32KHZ);
    AISetStreamVolLeft(0x12);
    AISetStreamVolRight(0x34);
    AISetStreamPlayState(AI_STREAM_STOP);
    check(AIGetStreamVolLeft() == 0x12 && AIGetStreamVolRight() == 0x34, "stream volume registers");
    check(AIGetStreamPlayState() == AI_STREAM_STOP, "stream play state register");
    const int opens = fake_ai::opens;
    AIReset();
    check(!AICheckInit(), "AIReset clears the init flag");
    AIInit(nullptr);
    check(AICheckInit() && fake_ai::opens == opens, "AIInit after AIReset keeps the open output");
}

}

int main() {
    dma();
    guest_addresses();
    sample_rate_and_stream();
    std::printf("%s\n", failures ? "ai: failures" : "ai: ok");
    return failures ? 1 : 0;
}
