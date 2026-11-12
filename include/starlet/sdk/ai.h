#pragma once

// The audio interface (AI) SDK types and functions starlet::ai implements, with the console SDK's
// signatures.

#include "starlet/sdk/os.h"

namespace starlet::sdk {

using AIDCallback = void (*)();
using AISCallback = void (*)(u32 count);

constexpr u32 AI_STREAM_STOP = 0;
constexpr u32 AI_STREAM_START = 1;
constexpr u32 AI_SAMPLERATE_32KHZ = 0;
constexpr u32 AI_SAMPLERATE_48KHZ = 1;

} // namespace starlet::sdk

#ifndef STARLET_SDK_TYPES_ONLY
extern "C" {
void AIInit(starlet::sdk::u8* stack);
starlet::sdk::BOOL AICheckInit();
void AIReset();
starlet::sdk::AIDCallback AIRegisterDMACallback(starlet::sdk::AIDCallback callback);
void AIInitDMA(starlet::sdk::u32 start_addr, starlet::sdk::u32 length);
void AIStartDMA();
void AIStopDMA();
starlet::sdk::BOOL AIGetDMAEnableFlag();
starlet::sdk::u32 AIGetDMAStartAddr();
starlet::sdk::u32 AIGetDMALength();
starlet::sdk::u32 AIGetDMABytesLeft();
starlet::sdk::u32 AIGetDSPSampleRate();
void AISetDSPSampleRate(starlet::sdk::u32 rate);
void AISetStreamPlayState(starlet::sdk::u32 state);
starlet::sdk::u32 AIGetStreamPlayState();
void AISetStreamVolLeft(starlet::sdk::u8 vol);
void AISetStreamVolRight(starlet::sdk::u8 vol);
starlet::sdk::u8 AIGetStreamVolLeft();
starlet::sdk::u8 AIGetStreamVolRight();
}
#endif
