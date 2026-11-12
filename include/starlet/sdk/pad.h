#pragma once

// The PAD SDK types and functions starlet::pad implements, with the console SDK's layouts.

#include "starlet/sdk/os.h"

namespace starlet::sdk {

struct PADStatus {
    u16 button;
    s8 stickX;
    s8 stickY;
    s8 substickX;
    s8 substickY;
    u8 triggerLeft;
    u8 triggerRight;
    u8 analogA;
    u8 analogB;
    s8 err;
};
static_assert(sizeof(PADStatus) == 12, "the SDK's PADStatus");

constexpr int PAD_CHANMAX = 4;
constexpr u32 PAD_CHAN0_BIT = 0x80000000;
constexpr s8 PAD_ERR_NONE = 0;
constexpr s8 PAD_ERR_NO_CONTROLLER = -1;

} // namespace starlet::sdk

#ifndef STARLET_SDK_TYPES_ONLY
extern "C" {
starlet::sdk::BOOL PADInit();
starlet::sdk::u32 PADRead(starlet::sdk::PADStatus* status);
starlet::sdk::BOOL PADReset(starlet::sdk::u32 mask);
starlet::sdk::BOOL PADRecalibrate(starlet::sdk::u32 mask);
void PADClamp(starlet::sdk::PADStatus* status);
void PADClampCircle(starlet::sdk::PADStatus* status);
void PADControlMotor(starlet::sdk::s32 chan, starlet::sdk::u32 command);
void PADSetAnalogMode(starlet::sdk::u32 mode);
}
#endif
