#pragma once

#include "starlet/host/pad.hpp"

#include <functional>
#include <memory>

namespace starlet::host {

// The handheld's SDL game controllers. SDL reads the firmware's mappings from
// SDL_GAMECONTROLLERCONFIG, which PortMaster sets, when the subsystem starts; a joystick
// without a mapping is reported and left closed.
//
// Slots fill with controllers in SDL's device order; a removed controller leaves its slot
// empty until another arrives. update() runs on the thread that pumps SDL's events; state()
// may run on any thread.
class Controllers {
public:
    // A line for the port's log: an opened or removed controller, or joysticks without a mapping.
    using Log = std::function<void(const char* line)>;

    // slots: how many controllers the game reads, 1 to 4.
    explicit Controllers(unsigned slots = 1, Log log = nullptr);
    ~Controllers();
    Controllers(const Controllers&) = delete;
    Controllers& operator=(const Controllers&) = delete;

    // Updates SDL's controller state, handles controllers added and removed, and samples
    // every slot.
    void update();
    // The latest sample; disconnected for an empty slot or one past slots.
    Gamepad state(unsigned slot) const;
    unsigned slots() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
