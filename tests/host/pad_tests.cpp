#include "starlet/host/controllers.hpp"
#include "starlet/host/pad.hpp"

#include <SDL.h>
#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace starlet::host;

namespace {

void require(bool value, const std::string& why) {
    if (!value) {
        std::fprintf(stderr, "pad: %s\n", why.c_str());
        std::exit(1);
    }
}

using Shape = StickResponse::Shape;

// Each port's layout as data, and its input code before Starlet, transcribed as the reference.

// Pikmin 2: face buttons by their printed Nintendo labels, shoulders as L and R, either
// trigger as Z, a radial dead zone (src/application/host_app.cpp, Controller).
GameCubeMapping pikmin2_mapping() {
    GameCubeMapping m;
    m.a = bit(Input::east);
    m.b = bit(Input::south);
    m.x = bit(Input::north);
    m.y = bit(Input::west);
    m.start = bit(Input::start);
    m.up = bit(Input::dpad_up);
    m.down = bit(Input::dpad_down);
    m.left = bit(Input::dpad_left);
    m.right = bit(Input::dpad_right);
    m.l = bit(Input::left_shoulder);
    m.r = bit(Input::right_shoulder);
    m.z = bit(Input::left_trigger) | bit(Input::right_trigger);
    m.trigger_threshold = 16000;
    m.trigger_left = {false, true};
    m.trigger_right = {false, true};
    m.main_stick = {Shape::radial, 8000, true, 32767.f, false, 72};
    m.c_stick = {Shape::radial, 8000, true, 32767.f, false, 59};
    return m;
}

void pikmin2_stick(std::int16_t ax, std::int16_t ay, int range, std::int8_t& x, std::int8_t& y) {
    constexpr float deadzone = 8000.f;
    float fx = ax, fy = -float(ay);
    const float length = std::hypot(fx, fy);
    if (length <= deadzone) {
        x = y = 0;
        return;
    }
    const float scale = std::min(1.f, (length - deadzone) / (32767.f - deadzone)) / length;
    x = static_cast<std::int8_t>(std::lround(fx * scale * range));
    y = static_cast<std::int8_t>(std::lround(fy * scale * range));
}

GameCubePad pikmin2_reference(const Gamepad& pad) {
    GameCubePad state;
    if (!pad.connected) return state;
    state.connected = true;
    const struct {
        Input input;
        std::uint16_t output;
    } buttons[] = {
        {Input::east, 0x0100},          {Input::south, 0x0200},     {Input::north, 0x0400},
        {Input::west, 0x0800},          {Input::start, 0x1000},     {Input::dpad_left, 0x0001},
        {Input::dpad_right, 0x0002},    {Input::dpad_down, 0x0004}, {Input::dpad_up, 0x0008},
        {Input::left_shoulder, 0x0040}, {Input::right_shoulder, 0x0020},
    };
    for (const auto& b : buttons)
        if (pad.buttons & bit(b.input)) state.buttons |= b.output;
    if (pad.left_trigger > 16000 || pad.right_trigger > 16000) state.buttons |= 0x0010;
    state.trigger_left = (state.buttons & 0x0040) ? 255 : 0;
    state.trigger_right = (state.buttons & 0x0020) ? 255 : 0;
    pikmin2_stick(pad.left_x, pad.left_y, 72, state.stick_x, state.stick_y);
    pikmin2_stick(pad.right_x, pad.right_y, 59, state.substick_x, state.substick_y);
    return state;
}

// gc-recomp: face buttons by GameCube position, the right shoulder as Z, triggers as analog
// L and R, a 12% axial dead zone without rescaling, sticks as SI bytes around 128
// (frontend/src/frontend.cpp, Pads).
GameCubeMapping gc_recomp_mapping() {
    GameCubeMapping m;
    m.a = bit(Input::south);
    m.b = bit(Input::west);
    m.x = bit(Input::east);
    m.y = bit(Input::north);
    m.start = bit(Input::start);
    m.z = bit(Input::right_shoulder);
    m.up = bit(Input::dpad_up);
    m.down = bit(Input::dpad_down);
    m.left = bit(Input::dpad_left);
    m.right = bit(Input::dpad_right);
    m.l = bit(Input::left_shoulder) | bit(Input::left_trigger);
    m.r = bit(Input::right_trigger);
    m.trigger_threshold = 24000;
    m.trigger_left = {true, true};
    m.trigger_right = {true, false};
    // |v| / 32768 < 0.12 holds up to 3932.
    m.main_stick = {Shape::axial, 3932, false, 32768.f, false, 100};
    m.c_stick = m.main_stick;
    return m;
}

struct SiPad {
    std::uint16_t buttons = 0;
    std::uint8_t stick_x = 128, stick_y = 128, cstick_x = 128, cstick_y = 128, trigger_l = 0, trigger_r = 0;
};

std::uint8_t gc_recomp_axis(std::int16_t v, bool invert) {
    double x = v / 32768.0;
    if (invert) x = -x;
    if (std::fabs(x) < 0.12) x = 0;
    return static_cast<std::uint8_t>(std::lround(128 + std::clamp(x, -1.0, 1.0) * 100));
}

SiPad gc_recomp_reference(const Gamepad& pad) {
    SiPad p;
    const auto held = [&](Input input) { return (pad.buttons & bit(input)) != 0; };
    std::uint16_t b = 0;
    if (held(Input::south)) b |= pad_button::a;
    if (held(Input::west)) b |= pad_button::b;
    if (held(Input::east)) b |= pad_button::x;
    if (held(Input::north)) b |= pad_button::y;
    if (held(Input::start)) b |= pad_button::start;
    if (held(Input::right_shoulder)) b |= pad_button::z;
    if (held(Input::dpad_up)) b |= pad_button::up;
    if (held(Input::dpad_down)) b |= pad_button::down;
    if (held(Input::dpad_left)) b |= pad_button::left;
    if (held(Input::dpad_right)) b |= pad_button::right;
    p.stick_x = gc_recomp_axis(pad.left_x, false);
    p.stick_y = gc_recomp_axis(pad.left_y, true);
    p.cstick_x = gc_recomp_axis(pad.right_x, false);
    p.cstick_y = gc_recomp_axis(pad.right_y, true);
    const std::int16_t l = pad.left_trigger, r = pad.right_trigger;
    p.trigger_l = static_cast<std::uint8_t>(std::clamp(l / 128, 0, 255));
    p.trigger_r = static_cast<std::uint8_t>(std::clamp(r / 128, 0, 255));
    if (held(Input::left_shoulder) || l > 24000) {
        b |= pad_button::l;
        p.trigger_l = 255;
    }
    if (r > 24000) b |= pad_button::r;
    p.buttons = b;
    return p;
}

// Galaxy reads the sticks itself for its Remote and Nunchuk: an axial dead zone with
// rescaling, its own circle limit later (src/platform/sdl_input.cpp, axis).
const StickResponse galaxy_stick{Shape::axial, 8000, true, 32768.f, false, 71};

float galaxy_axis(int value) {
    constexpr int deadzone = 8000;
    if (std::abs(value) <= deadzone) return 0;
    return float(value > 0 ? value - deadzone : value + deadzone) / float(32768 - deadzone);
}

bool same(const GameCubePad& a, const GameCubePad& b) {
    return a.connected == b.connected && a.buttons == b.buttons && a.stick_x == b.stick_x && a.stick_y == b.stick_y &&
           a.substick_x == b.substick_x && a.substick_y == b.substick_y && a.trigger_left == b.trigger_left &&
           a.trigger_right == b.trigger_right;
}

std::string describe(std::int16_t x, std::int16_t y) { return "(" + std::to_string(x) + ", " + std::to_string(y) + ")"; }

// Every value of one axis against a few of the other, then random pairs.
template <class Check> void sweep(Check check) {
    const std::int16_t others[] = {0, 1, -1, 3932, -3933, 4096, -4096, 5700, 8000, -8001, 20000, -24000, 32767, -32768};
    for (int v = -32768; v <= 32767; ++v)
        for (std::int16_t other : others) {
            check(std::int16_t(v), other);
            check(other, std::int16_t(v));
        }
    std::mt19937 random(3);
    for (int i = 0; i < 500000; ++i) check(std::int16_t(random()), std::int16_t(random()));
}

void pikmin2_reproduced() {
    const auto mapping = pikmin2_mapping();
    sweep([&](std::int16_t x, std::int16_t y) {
        Gamepad pad;
        pad.connected = true;
        pad.left_x = x;
        pad.left_y = y;
        pad.right_x = y;
        pad.right_y = x;
        require(same(to_gamecube(pad, mapping), pikmin2_reference(pad)), "Pikmin 2 sticks at " + describe(x, y));
    });
    const std::int16_t triggers[] = {0, 16000, 16001, 24000, 24001, 32767};
    for (std::uint32_t buttons = 0; buttons < 1u << 15; ++buttons)
        for (std::int16_t l : triggers)
            for (std::int16_t r : {std::int16_t(0), std::int16_t(32767)}) {
                Gamepad pad;
                pad.connected = true;
                pad.buttons = buttons;
                pad.left_trigger = l;
                pad.right_trigger = r;
                require(same(to_gamecube(pad, mapping), pikmin2_reference(pad)), "Pikmin 2 buttons " + std::to_string(buttons));
            }
}

// gc-recomp rounds 128 + x * 100, so an exact half below the centre rounds up; Starlet rounds
// the signed value half away from zero. Only the raw values whose x * 100 is -n.5 differ, by one.
void gc_recomp_reproduced() {
    const auto mapping = gc_recomp_mapping();
    const auto half_below = [](int up) { return up < 0 && (-up) % 8192 == 4096; };
    unsigned halves = 0;
    sweep([&](std::int16_t x, std::int16_t y) {
        Gamepad pad;
        pad.connected = true;
        pad.left_x = x;
        pad.left_y = y;
        pad.right_x = y;
        pad.right_y = x;
        const GameCubePad ours = to_gamecube(pad, mapping);
        const SiPad theirs = gc_recomp_reference(pad);
        const auto agrees = [&](std::int8_t value, std::uint8_t si, int up) {
            if (half_below(up)) {
                ++halves;
                return 128 + value == si - 1;
            }
            return 128 + value == si;
        };
        require(agrees(ours.stick_x, theirs.stick_x, x) && agrees(ours.stick_y, theirs.stick_y, -int(y)) &&
                    agrees(ours.substick_x, theirs.cstick_x, y) && agrees(ours.substick_y, theirs.cstick_y, -int(x)),
                "gc-recomp sticks at " + describe(x, y));
    });
    require(halves > 0, "the sweep met the rounding difference");
    for (std::uint32_t buttons = 0; buttons < 1u << 15; ++buttons)
        for (std::int16_t l : {0, 127, 128, 16000, 24000, 24001, 32767})
            for (std::int16_t r : {0, 200, 24000, 24001, 32767}) {
                Gamepad pad;
                pad.connected = true;
                pad.buttons = buttons;
                pad.left_trigger = l;
                pad.right_trigger = r;
                const GameCubePad ours = to_gamecube(pad, mapping);
                const SiPad theirs = gc_recomp_reference(pad);
                require(ours.buttons == theirs.buttons && ours.trigger_left == theirs.trigger_l &&
                            ours.trigger_right == theirs.trigger_r,
                        "gc-recomp buttons " + std::to_string(buttons) + " triggers " + describe(l, r));
            }
}

void galaxy_reproduced() {
    sweep([&](std::int16_t x, std::int16_t y) {
        const Stick stick = read_stick(x, y, galaxy_stick);
        require(stick.x == galaxy_axis(x) && stick.y == -galaxy_axis(y), "Galaxy's stick axes at " + describe(x, y));
    });
}

void dead_zones() {
    const auto pikmin = pikmin2_mapping().main_stick;
    const auto gc = gc_recomp_mapping().main_stick;
    // Length 8061: past Pikmin 2's radial dead zone, inside Galaxy's per-axis one.
    const Stick diagonal = read_stick(5700, -5700, pikmin);
    require(diagonal.x > 0 && diagonal.y > 0 && diagonal.x < .01f, "a radial dead zone measures the length");
    const Stick axial = read_stick(5700, -5700, galaxy_stick);
    require(axial.x == 0 && axial.y == 0, "an axial dead zone measures each axis");
    require(read_stick(8000, 0, pikmin).x == 0 && read_stick(8001, 0, pikmin).x > 0, "the dead zone's edge reads 0");
    require(stick_value(read_stick(8100, 0, pikmin).x, 72) == 0, "rescaled values start at 0");
    require(stick_value(read_stick(3933, 0, gc).x, 100) == 12, "without rescaling, values jump at the edge");
    require(stick_value(read_stick(32767, 0, pikmin).x, 72) == 72 && stick_value(read_stick(-32768, 0, pikmin).x, 72) == -72,
            "full deflection reads the range");
    require(stick_value(read_stick(0, -32768, pikmin).y, 72) == 72, "SDL's up is the console's positive y");
    const Stick corner = read_stick(32767, 32767, pikmin);
    require(stick_value(corner.x, 72) == 51 && stick_value(corner.y, 72) == -51, "the radial response stays within the circle");
    StickResponse limited = galaxy_stick;
    limited.circle = true;
    const Stick square = read_stick(32767, 32767, galaxy_stick), round = read_stick(32767, 32767, limited);
    require(square.x > .99f && std::fabs(std::hypot(round.x, round.y) - 1) < 1e-6f, "circle limits an axial corner");
    require(stick_value(1, 200) == 127 && stick_value(-1, 200) == -128, "values stay within a signed byte");
}

void layouts() {
    Gamepad pad;
    pad.connected = true;
    pad.buttons = bit(Input::east);
    require(to_gamecube(pad, pikmin2_mapping()).buttons == pad_button::a, "Pikmin 2 reads the right button as A");
    require(to_gamecube(pad, gc_recomp_mapping()).buttons == pad_button::x, "gc-recomp reads it as X");
    pad.buttons = 0;
    pad.left_trigger = 16001;
    require(pressed(pad, Input::left_trigger, 16000) && !pressed(pad, Input::left_trigger, 16001), "a trigger presses above its threshold");
    require(to_gamecube(pad, pikmin2_mapping()).buttons == pad_button::z, "Pikmin 2's Z");
    require(to_gamecube(pad, gc_recomp_mapping()).buttons == 0 && to_gamecube(pad, gc_recomp_mapping()).trigger_left == 125,
            "gc-recomp's analog L below its threshold");
    pad.connected = false;
    pad.buttons = ~0u;
    require(same(to_gamecube(pad, pikmin2_mapping()), GameCubePad{}), "a disconnected pad reads nothing");
}

// The firmware path: a joystick SDL has no mapping for, the mapping a frontend supplies, and
// controllers arriving and leaving. SDL 2.0.14 runtimes provide virtual joysticks.
using Attach = int (*)(SDL_JoystickType, int, int, int);
using Detach = int (*)(int);
using SetButton = int (*)(SDL_Joystick*, int, Uint8);
using SetAxis = int (*)(SDL_Joystick*, int, Sint16);

// SDL 2.0.9's buttons and axes; newer headers count more buttons.
constexpr int button_count = int(Input::dpad_right) + 1, axis_count = 6;

int controllers() {
    const auto attach = reinterpret_cast<Attach>(dlsym(RTLD_DEFAULT, "SDL_JoystickAttachVirtual"));
    const auto detach = reinterpret_cast<Detach>(dlsym(RTLD_DEFAULT, "SDL_JoystickDetachVirtual"));
    const auto set_button = reinterpret_cast<SetButton>(dlsym(RTLD_DEFAULT, "SDL_JoystickSetVirtualButton"));
    const auto set_axis = reinterpret_cast<SetAxis>(dlsym(RTLD_DEFAULT, "SDL_JoystickSetVirtualAxis"));
    if (!attach || !detach || !set_button || !set_axis) {
        std::puts("SKIP: this SDL has no virtual joysticks");
        return 77;
    }
    // Only the virtual joysticks: the host's own controllers stay out.
    for (const char* hint : {"SDL_JOYSTICK_HIDAPI", "SDL_JOYSTICK_IOKIT", "SDL_JOYSTICK_MFI", "SDL_JOYSTICK_GAMEINPUT"})
        setenv(hint, "0", 1);
    std::vector<std::string> lines;
    const auto logged = [&](const std::string& text) {
        for (const auto& line : lines)
            if (line.find(text) != std::string::npos) return true;
        return false;
    };
    Controllers pads(2, [&](const char* line) { lines.push_back(line); });
    if (SDL_NumJoysticks() != 0) {
        std::puts("SKIP: the host has joysticks attached");
        return 77;
    }
    pads.update();
    require(!pads.state(0).connected && logged("none of 0 joysticks has an SDL mapping"), "no controller, reported");

    const auto plug = [&](SDL_Joystick*& joystick) {
        const int index = attach(SDL_JOYSTICK_TYPE_UNKNOWN, axis_count, button_count, 0);
        require(index >= 0, std::string("a virtual joystick: ") + SDL_GetError());
        joystick = SDL_JoystickOpen(index);
        require(joystick, "the virtual joystick opens");
        for (int axis : {SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_CONTROLLER_AXIS_TRIGGERRIGHT}) set_axis(joystick, axis, -32768);
        return index;
    };
    SDL_Joystick* first = nullptr;
    const int index = plug(first);
    SDL_JoystickGUID id = SDL_JoystickGetDeviceGUID(index);
    id.data[2] = id.data[3] = 0;
    char guid[33];
    SDL_JoystickGetGUIDString(id, guid, sizeof guid);
    const bool unmapped = !SDL_IsGameController(index);
    if (unmapped) {
        pads.update();
        require(!pads.state(0).connected && logged(std::string("none of 1 joysticks has an SDL mapping: ") + guid),
                "an unmapped joystick is reported with the GUID its mapping needs");
    }
    // The line PortMaster's SDL_GAMECONTROLLERCONFIG would carry: SDL's buttons and axes in order.
    std::string mapping = std::string(guid) + ",Starlet virtual pad,";
    const char* buttons[] = {"a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
                             "rightshoulder", "dpup", "dpdown", "dpleft", "dpright"};
    const char* axes[] = {"leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"};
    for (int i = 0; i < button_count; ++i) mapping += std::string(buttons[i]) + ":b" + std::to_string(i) + ",";
    for (int i = 0; i < axis_count; ++i) mapping += std::string(axes[i]) + ":a" + std::to_string(i) + ",";
    require(SDL_GameControllerAddMapping(mapping.c_str()) >= 0, std::string("the mapping is accepted: ") + SDL_GetError());

    pads.update();
    require(pads.state(0).connected && !pads.state(1).connected && logged("Controller 1: "), "the mapped joystick opens in slot 1");
    set_button(first, SDL_CONTROLLER_BUTTON_B, 1);
    set_button(first, SDL_CONTROLLER_BUTTON_DPAD_UP, 1);
    set_axis(first, SDL_CONTROLLER_AXIS_LEFTX, 32767);
    set_axis(first, SDL_CONTROLLER_AXIS_LEFTY, -32768);
    set_axis(first, SDL_CONTROLLER_AXIS_RIGHTY, 20000);
    set_axis(first, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
    pads.update();
    const Gamepad state = pads.state(0);
    require(state.buttons == (bit(Input::east) | bit(Input::dpad_up)), "buttons by SDL's names");
    require(state.left_x == 32767 && state.left_y == -32768 && state.right_x == 0 && state.right_y == 20000,
            "stick axes as SDL reports them");
    require(state.left_trigger == 32767 && state.right_trigger == 0, "triggers from 0 to 32767");
    const GameCubePad gc = to_gamecube(state, pikmin2_mapping());
    require(gc.buttons == (pad_button::a | pad_button::up | pad_button::z), "Pikmin 2's layout on the controller");
    require(gc.stick_x == 51 && gc.stick_y == 51 && gc.substick_y < 0 && gc.substick_x == 0, "Pikmin 2's sticks on the controller");

    SDL_Joystick* second = nullptr;
    const int second_index = plug(second);
    pads.update();
    require(pads.state(1).connected && logged("Controller 2: "), "a second controller fills slot 2");
    (void)second_index;

    lines.clear();
    SDL_JoystickClose(first);
    detach(index);
    pads.update();
    require(!pads.state(0).connected && pads.state(1).connected && logged("Controller 1: removed"),
            "a removed controller empties its slot and keeps the others");
    SDL_Joystick* third = nullptr;
    const int third_index = plug(third);
    pads.update();
    require(pads.state(0).connected && logged("Controller 1: "), "a new controller fills the empty slot");

    SDL_JoystickClose(second);
    SDL_JoystickClose(third);
    for (int i = SDL_NumJoysticks() - 1; i >= 0; --i) detach(i);
    pads.update();
    require(!pads.state(0).connected && !pads.state(1).connected && !pads.state(2).connected, "every controller removed");
    (void)third_index;
    std::printf("controllers: %sfirmware mappings, slots across hotplug\n", unmapped ? "an unmapped joystick reported, " : "");
    return 0;
}

}

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "controllers")) return controllers();
    pikmin2_reproduced();
    gc_recomp_reproduced();
    galaxy_reproduced();
    dead_zones();
    layouts();
    std::puts("pad: Pikmin 2, gc-recomp and Galaxy input reproduced; dead zones and layouts");
    return 0;
}
