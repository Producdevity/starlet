// GameCube controller reads from the application's input sampling.
#include "starlet/pad/pad.hpp"

#include "starlet/sdk/pad.h"

#include <cstring>
#include <mutex>

using namespace starlet::sdk;

namespace starlet::pad {
namespace {

std::mutex mutex;
PADStatus latest[PAD_CHANMAX];
bool connected[PAD_CHANMAX];
void (*read_hook)() = nullptr;

}

void set_read_hook(void (*hook)()) { read_hook = hook; }

void publish(unsigned channel, const PadState& state) {
    if (channel >= PAD_CHANMAX) return;
    std::lock_guard<std::mutex> lock(mutex);
    connected[channel] = state.connected;
    PADStatus& s = latest[channel];
    std::memset(&s, 0, sizeof s);
    if (!state.connected) {
        s.err = PAD_ERR_NO_CONTROLLER;
        return;
    }
    s.button = state.buttons;
    s.stickX = state.stick_x;
    s.stickY = state.stick_y;
    s.substickX = state.substick_x;
    s.substickY = state.substick_y;
    s.triggerLeft = state.trigger_left;
    s.triggerRight = state.trigger_right;
    s.err = PAD_ERR_NONE;
}

}

using namespace starlet::pad;

extern "C" {

BOOL PADInit() {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& s : latest) {
        std::memset(&s, 0, sizeof s);
        s.err = PAD_ERR_NO_CONTROLLER;
    }
    return TRUE;
}

BOOL PADReset(u32) { return TRUE; }
BOOL PADRecalibrate(u32) { return TRUE; }
BOOL PADSync() { return TRUE; }
void PADSetAnalogMode(u32) {}
void PADSetSpec(u32) {}
void PADSetSamplingRate(u32) {}
void PADControlMotor(s32, u32) {}
void PADControlAllMotors(u32*) {}

u32 PADRead(PADStatus* status) {
    if (read_hook) read_hook();
    std::lock_guard<std::mutex> lock(mutex);
    u32 motor = 0;
    for (int i = 0; i < PAD_CHANMAX; ++i) {
        status[i] = latest[i];
        if (connected[i]) motor |= PAD_CHAN0_BIT >> i;
    }
    return motor;
}

// Sticks and triggers already arrive within the console's calibrated ranges,
// with the dead zone applied by the input sampler.
void PADClamp(PADStatus*) {}
void PADClampCircle(PADStatus*) {}

}
