#pragma once

#include <cstdint>

namespace starlet::host {

// A handheld control. The buttons are SDL's game-controller buttons in SDL's order, the face
// buttons named by place: SDL calls them A, B, X and Y after the Xbox layout. The triggers
// are analog and count as pressed past a threshold the mapping gives.
enum class Input : std::uint8_t {
    south,  // SDL A
    east,   // SDL B
    west,   // SDL X
    north,  // SDL Y
    back,
    guide,
    start,
    left_stick,
    right_stick,
    left_shoulder,
    right_shoulder,
    dpad_up,
    dpad_down,
    dpad_left,
    dpad_right,
    left_trigger,
    right_trigger,
};
constexpr std::uint32_t bit(Input input) { return 1u << unsigned(input); }

// One controller as SDL reports it after the firmware's mapping.
struct Gamepad {
    bool connected = false;
    std::uint32_t buttons = 0;  // bit(Input) of each held button, south to dpad_right
    // -32768..32767, positive right and down.
    std::int16_t left_x = 0, left_y = 0, right_x = 0, right_y = 0;
    std::int16_t left_trigger = 0, right_trigger = 0;  // 0..32767
};
// Whether input is held; a trigger counts above trigger_threshold.
bool pressed(const Gamepad& pad, Input input, std::int16_t trigger_threshold);

// How a stick's raw deflection becomes the game's. Each port keeps its own.
struct StickResponse {
    enum class Shape : std::uint8_t {
        radial,  // the dead zone and scaling apply to the stick's length
        axial,   // they apply to each axis alone
    };
    Shape shape = Shape::radial;
    int dead_zone = 0;     // raw deflection at or below this reads 0
    bool rescale = false;  // values start at 0 at the dead zone's edge instead of jumping to it
    float full = 32767;    // raw deflection that reads as 1
    bool circle = false;   // limit the length to 1
    int range = 0;         // the console's value for a deflection of 1
};
// Up is positive, so y is SDL's negated. Each component lies within -1..1.
struct Stick {
    float x = 0, y = 0;
};
Stick read_stick(std::int16_t x, std::int16_t y, const StickResponse& response);
// The console's signed value: x * range rounded half away from zero.
std::int8_t stick_value(float value, int range);

// PAD_BUTTON_* bits as PADStatus reports them.
namespace pad_button {
constexpr std::uint16_t left = 0x0001, right = 0x0002, down = 0x0004, up = 0x0008, z = 0x0010, r = 0x0020, l = 0x0040,
                        a = 0x0100, b = 0x0200, x = 0x0400, y = 0x0800, start = 0x1000;
}

// A GameCube controller as PADRead reports it: sticks within the console's signed range, up
// positive; analog triggers 0-255.
struct GameCubePad {
    bool connected = false;
    std::uint16_t buttons = 0;
    std::int8_t stick_x = 0, stick_y = 0, substick_x = 0, substick_y = 0;
    std::uint8_t trigger_left = 0, trigger_right = 0;
};

// A port's GameCube layout on the handheld. Each button field holds the bit(Input) of the
// controls that press it.
struct GameCubeMapping {
    std::uint32_t a = 0, b = 0, x = 0, y = 0, z = 0, l = 0, r = 0, start = 0;
    std::uint32_t up = 0, down = 0, left = 0, right = 0;
    std::int16_t trigger_threshold = 16000;
    // An analog L or R: from_axis reads its trigger's axis, 0..32767 as 0..255; full_when_pressed
    // reads 255 while the digital L or R is pressed.
    struct Analog {
        bool from_axis = false;
        bool full_when_pressed = true;
    };
    Analog trigger_left, trigger_right;
    StickResponse main_stick, c_stick;  // the left and right sticks
};
GameCubePad to_gamecube(const Gamepad& pad, const GameCubeMapping& mapping);

}
