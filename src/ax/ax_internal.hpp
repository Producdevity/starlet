#pragma once

// The DSP side of AX: what one 5 ms frame of the AX microcode does with the parameter blocks and
// the command list the CPU side prepared. ax_sdk.cpp owns the CPU side and drives frames.

#include "starlet/sdk/ax.h"

#include <cstddef>
#include <cstdint>

namespace starlet::ax::detail {

using namespace starlet::sdk;

constexpr int ms_samples = 32;
constexpr int frame_ms = 5;
constexpr int frame_samples = ms_samples * frame_ms;
constexpr int update_words = 128;

// The DSP's nine mixing buses, in the order of the studio parameter block.
enum Bus { main_l, main_r, main_s, auxa_l, auxa_r, auxa_s, auxb_l, auxb_r, auxb_s, bus_count };

// The aux buffers in main memory the DSP and the aux callbacks exchange: three per bus group,
// 160 samples each of L, R and S.
struct AuxBuffers {
    s32 a[3][3 * frame_samples];
    s32 b[3][3 * frame_samples];
};

// What __AXNextFrame encodes in a command list, and the studio block it points at.
struct FrameCommand {
    u32 mode = AX_MODE_STEREO;
    bool aux_a = false;
    bool aux_b = false;
    bool compressor = false;
    u32 aux_write = 0, aux_read = 1;
    u32 dpl2_write = 0, dpl2_read = 1;
    u32 output = 0;  // the AI buffer the frame is mixed into
    s32 studio_value[bus_count] = {};
    s16 studio_delta[bus_count] = {};
};

struct Dsp {
    AXPB pb[AX_MAX_VOICES];
    u16 updates[AX_MAX_VOICES][update_words];
    s32 surround[frame_samples];  // the S bus of the previous frame
    int bus[bus_count][frame_samples];
    u16 compressor_pos;
};

struct Memory {
    const std::uint8_t* aram;
    std::size_t aram_bytes;
};

void reset(Dsp& dsp);

// One frame: setup, voices, aux exchange, compressor, output. out takes 160 stereo frames, right
// channel first, as the AI DMA reads them.
void run_frame(Dsp& dsp, const FrameCommand& command, AuxBuffers& aux, const Memory& memory,
               const u16* compressor_table, s16* out);

// The accelerator and sample-rate converter for one voice: count output samples at 32 kHz.
// Updates the block's addresses, decoder state and converter state as the DSP does.
void read_samples(AXPB& pb, const Memory& memory, s16* out, u32 count);

// The 4-tap converter's coefficients: four tables (8, 12 and 16 kHz, then zeros) of 128 phases.
const s16* src_coefficients();

}
