#pragma once

#include <cstdint>

namespace starlet::pad {

// One controller as the console reports it: PAD_BUTTON_* bits, sticks within the calibrated range a
// real controller reaches (about +-72 main, +-59 C), analog triggers 0-255.
struct PadState {
    bool connected = false;
    std::uint16_t buttons = 0;
    std::int8_t stick_x = 0, stick_y = 0, substick_x = 0, substick_y = 0;
    std::uint8_t trigger_left = 0, trigger_right = 0;
};

// Input sampling (a host thread) publishes; PADRead (guest) copies the latest.
void publish(unsigned channel, const PadState&);

// Called on every PADRead, before the copy (development hooks).
void set_read_hook(void (*hook)());

}
