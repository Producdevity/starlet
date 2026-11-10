#include "starlet/host/controllers.hpp"

#include <SDL.h>

#include <array>
#include <mutex>
#include <stdexcept>
#include <string>

namespace starlet::host {
namespace {

constexpr unsigned max_slots = 4;

Gamepad sample(SDL_GameController* controller) {
    Gamepad pad;
    pad.connected = true;
    for (unsigned i = 0; i <= unsigned(Input::dpad_right); ++i)
        if (SDL_GameControllerGetButton(controller, SDL_GameControllerButton(i))) pad.buttons |= bit(Input(i));
    pad.left_x = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
    pad.left_y = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
    pad.right_x = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX);
    pad.right_y = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY);
    pad.left_trigger = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    pad.right_trigger = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    return pad;
}

}

struct Controllers::Impl {
    unsigned slots = 1;
    Log log;
    std::array<SDL_GameController*, max_slots> open{};
    mutable std::mutex mutex;
    std::array<Gamepad, max_slots> latest{};
    // The joystick count last reported without a mapping; -1 once a controller opens.
    int reported_unmapped = -1;

    void say(const std::string& line) const {
        if (log) log(line.c_str());
    }

    bool is_open(SDL_JoystickID id) const {
        for (unsigned slot = 0; slot < slots; ++slot)
            if (open[slot] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(open[slot])) == id) return true;
        return false;
    }

    // GUID and name of each joystick, for an SDL_GAMECONTROLLERCONFIG line. Mappings carry
    // GUIDs without the CRC that newer SDL keeps in bytes 2 and 3.
    void report_unmapped() {
        const int count = SDL_NumJoysticks();
        if (count == reported_unmapped) return;
        reported_unmapped = count;
        std::string joysticks;
        for (int i = 0; i < count; ++i) {
            SDL_JoystickGUID id = SDL_JoystickGetDeviceGUID(i);
            id.data[2] = id.data[3] = 0;
            char guid[33];
            SDL_JoystickGetGUIDString(id, guid, sizeof guid);
            const char* name = SDL_JoystickNameForIndex(i);
            joysticks += std::string(i ? ", " : ": ") + guid + " " + (name ? name : "unnamed");
        }
        say("Controller: none of " + std::to_string(count) + " joysticks has an SDL mapping" + joysticks);
    }

    void attach() {
        const int count = SDL_NumJoysticks();
        for (int index = 0; index < count; ++index) {
            unsigned slot = 0;
            while (slot < slots && open[slot]) ++slot;
            if (slot == slots) return;
            if (!SDL_IsGameController(index) || is_open(SDL_JoystickGetDeviceInstanceID(index))) continue;
            SDL_GameController* controller = SDL_GameControllerOpen(index);
            if (!controller) {
                const char* name = SDL_JoystickNameForIndex(index);
                say(std::string("Controller: cannot open ") + (name ? name : "unnamed") + ": " + SDL_GetError());
                continue;
            }
            open[slot] = controller;
            char* mapping = SDL_GameControllerMapping(controller);
            const char* name = SDL_GameControllerName(controller);
            say("Controller " + std::to_string(slot + 1) + ": " + (mapping ? mapping : name ? name : "unnamed"));
            SDL_free(mapping);
        }
    }

    void update() {
        SDL_GameControllerUpdate();
        for (unsigned slot = 0; slot < slots; ++slot)
            if (open[slot] && !SDL_GameControllerGetAttached(open[slot])) {
                SDL_GameControllerClose(open[slot]);
                open[slot] = nullptr;
                say("Controller " + std::to_string(slot + 1) + ": removed");
            }
        attach();
        bool any = false;
        std::array<Gamepad, max_slots> sampled{};
        for (unsigned slot = 0; slot < slots; ++slot)
            if (open[slot]) {
                sampled[slot] = sample(open[slot]);
                any = true;
            }
        if (any) reported_unmapped = -1;
        else report_unmapped();
        std::lock_guard<std::mutex> lock(mutex);
        latest = sampled;
    }
};

Controllers::Controllers(unsigned slots, Log log) : impl_(std::make_unique<Impl>()) {
    if (slots < 1 || slots > max_slots) throw std::invalid_argument("controllers: 1 to 4 slots");
    impl_->slots = slots;
    impl_->log = std::move(log);
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        throw std::runtime_error(std::string("SDL game controllers: ") + SDL_GetError());
}

Controllers::~Controllers() {
    for (SDL_GameController* controller : impl_->open)
        if (controller) SDL_GameControllerClose(controller);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

void Controllers::update() { impl_->update(); }

Gamepad Controllers::state(unsigned slot) const {
    if (slot >= impl_->slots) return {};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->latest[slot];
}

unsigned Controllers::slots() const noexcept { return impl_->slots; }

}
