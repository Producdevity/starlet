#include "starlet/host/pad.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace starlet::host {
namespace {

float axis(int value, const StickResponse& response) {
    if (std::abs(value) <= response.dead_zone) return 0;
    if (!response.rescale) return std::clamp(float(value) / response.full, -1.f, 1.f);
    const int live = value > 0 ? value - response.dead_zone : value + response.dead_zone;
    return std::clamp(float(live) / (response.full - float(response.dead_zone)), -1.f, 1.f);
}

std::uint8_t analog_trigger(std::int16_t value) { return std::uint8_t(std::clamp(value / 128, 0, 255)); }

}

bool pressed(const Gamepad& pad, Input input, std::int16_t trigger_threshold) {
    if (input == Input::left_trigger) return pad.left_trigger > trigger_threshold;
    if (input == Input::right_trigger) return pad.right_trigger > trigger_threshold;
    return pad.buttons & bit(input);
}

Stick read_stick(std::int16_t x, std::int16_t y, const StickResponse& response) {
    // Negated as an int, so SDL's -32768 reads as full up.
    const int raw_x = x, raw_y = -int(y);
    Stick stick;
    if (response.shape == StickResponse::Shape::axial) {
        stick = {axis(raw_x, response), axis(raw_y, response)};
    } else {
        const float fx = float(raw_x), fy = float(raw_y);
        const float length = std::hypot(fx, fy);
        if (length <= float(response.dead_zone)) return {};
        const float live = response.rescale ? (length - float(response.dead_zone)) / (response.full - float(response.dead_zone))
                                            : length / response.full;
        const float scale = std::min(1.f, live) / length;
        stick = {fx * scale, fy * scale};
    }
    if (response.circle) {
        const float length = std::hypot(stick.x, stick.y);
        if (length > 1) stick = {stick.x / length, stick.y / length};
    }
    return stick;
}

std::int8_t stick_value(float value, int range) {
    return std::int8_t(std::clamp(std::lround(value * float(range)), -128l, 127l));
}

GameCubePad to_gamecube(const Gamepad& pad, const GameCubeMapping& mapping) {
    GameCubePad out;
    if (!pad.connected) return out;
    out.connected = true;
    const auto any = [&](std::uint32_t inputs) {
        for (unsigned i = 0; i <= unsigned(Input::right_trigger); ++i)
            if (inputs >> i & 1 && pressed(pad, Input(i), mapping.trigger_threshold)) return true;
        return false;
    };
    const struct {
        std::uint32_t inputs;
        std::uint16_t button;
    } buttons[] = {
        {mapping.a, pad_button::a},         {mapping.b, pad_button::b},       {mapping.x, pad_button::x},
        {mapping.y, pad_button::y},         {mapping.z, pad_button::z},       {mapping.l, pad_button::l},
        {mapping.r, pad_button::r},         {mapping.start, pad_button::start}, {mapping.up, pad_button::up},
        {mapping.down, pad_button::down},   {mapping.left, pad_button::left}, {mapping.right, pad_button::right},
    };
    for (const auto& b : buttons)
        if (any(b.inputs)) out.buttons |= b.button;
    const auto trigger = [&](const GameCubeMapping::Analog& analog, std::int16_t axis_value, std::uint16_t button) {
        std::uint8_t value = analog.from_axis ? analog_trigger(axis_value) : 0;
        if (analog.full_when_pressed && out.buttons & button) value = 255;
        return value;
    };
    out.trigger_left = trigger(mapping.trigger_left, pad.left_trigger, pad_button::l);
    out.trigger_right = trigger(mapping.trigger_right, pad.right_trigger, pad_button::r);
    const Stick main = read_stick(pad.left_x, pad.left_y, mapping.main_stick);
    const Stick c = read_stick(pad.right_x, pad.right_y, mapping.c_stick);
    out.stick_x = stick_value(main.x, mapping.main_stick.range);
    out.stick_y = stick_value(main.y, mapping.main_stick.range);
    out.substick_x = stick_value(c.x, mapping.c_stick.range);
    out.substick_y = stick_value(c.y, mapping.c_stick.range);
    return out;
}

}
