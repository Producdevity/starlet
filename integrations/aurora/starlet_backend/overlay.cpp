#include "overlay.hpp"

#ifdef STARLET_WITH_OVERLAY
#include <GLES3/gl31.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_video.h>
#include <spo.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <utility>

#include "aurora_host.hpp"

namespace aurora::starlet_backend::overlay {
namespace {
Module Log("aurora::starlet_backend::overlay");

std::mutex g_lock;
std::atomic<bool> g_ready{false};
bool g_failed = false;

// SDL_GamepadButton order, SOUTH to TOUCHPAD.
constexpr std::array<spo_button, 21> kButtons{
    SPO_SOUTH, SPO_EAST, SPO_WEST,    SPO_NORTH,   SPO_SELECT,  SPO_GUIDE,   SPO_START,
    SPO_L3,    SPO_R3,   SPO_L1,      SPO_R1,      SPO_UP,      SPO_DOWN,    SPO_LEFT,
    SPO_RIGHT, SPO_MISC, SPO_PADDLE1, SPO_PADDLE2, SPO_PADDLE3, SPO_PADDLE4, SPO_TOUCHPAD};
// SDL_GamepadAxis order.
constexpr std::array<spo_axis, 6> kAxes{SPO_LX, SPO_LY, SPO_RX, SPO_RY, SPO_LT, SPO_RT};

void* resolve(const char* name, void*) { return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name)); }

// Gives the overlay the held buttons and axes of a controller that was connected before it started.
void sync_gamepad(SDL_JoystickID id) {
  SDL_Gamepad* pad = SDL_GetGamepadFromID(id);
  if (pad == nullptr) {
    spo_input_reset(id);
    return;
  }
  std::uint32_t held = 0;
  for (std::size_t i = 0; i < kButtons.size(); ++i) {
    if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i))) {
      held |= 1u << kButtons[i];
    }
  }
  std::array<std::int32_t, SPO_AXIS_COUNT> values{};
  for (std::size_t i = 0; i < kAxes.size(); ++i) {
    values[kAxes[i]] = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i));
  }
  spo_input_sync(id, held, values.data());
}

bool start() {
  spo_config config{};
  config.struct_size = sizeof config;
  config.abi_version = SPO_ABI_VERSION;
  config.get_proc_address = resolve;
  config.config_path = "conf/overlay.ini";
  const char* name = std::getenv("STARLET_OVERLAY_NAME");
  config.port_name = name != nullptr && *name != '\0' ? name : "Aurora port";
  config.port_version = "Starlet renderer";
  if (const spo_result result = spo_init(&config); result != SPO_OK) {
    Log.error("overlay: spo_init failed ({})", static_cast<int>(result));
    return false;
  }
  int count = 0;
  if (SDL_JoystickID* pads = SDL_GetGamepads(&count)) {
    for (int i = 0; i < count; ++i) {
      sync_gamepad(pads[i]);
    }
    SDL_free(pads);
  }
  Log.info("overlay: sbc-port-overlay {}", spo_version());
  return true;
}
} // namespace

void frame(int width, int height) {
  std::lock_guard lock(g_lock);
  if (!g_ready.load()) {
    if (g_failed) {
      return;
    }
    g_failed = !start();
    if (g_failed) {
      return;
    }
    g_ready = true;
  }
  // The overlay assumes the default state of the window's framebuffer and of texture unit 0;
  // the renderer's last pass leaves neither.
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  const GLenum back = GL_BACK;
  glDrawBuffers(1, &back);
  glActiveTexture(GL_TEXTURE0);
  if (const spo_result result = spo_frame(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
      result != SPO_OK) {
    static bool reported = false;
    if (!std::exchange(reported, true)) {
      Log.warn("overlay: spo_frame failed ({}, last error {})", static_cast<int>(result), static_cast<int>(spo_last_error()));
    }
  }
  if (!spo_input_captured() && spo_config_dirty()) {
    spo_config_save();
  }
}

bool event(const SDL_Event& e) {
  if (!g_ready.load()) {
    return false;
  }
  std::lock_guard lock(g_lock);
  switch (e.type) {
  case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
  case SDL_EVENT_GAMEPAD_BUTTON_UP:
    return spo_handle_input(e.gbutton.which, e.gbutton.button < kButtons.size() ? kButtons[e.gbutton.button] : SPO_OTHER,
                            e.gbutton.down ? 1 : 0) != 0;
  case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    if (e.gaxis.axis >= kAxes.size()) {
      return spo_input_captured() != 0;
    }
    return spo_handle_axis(e.gaxis.which, kAxes[e.gaxis.axis], e.gaxis.value) != 0;
  case SDL_EVENT_GAMEPAD_ADDED:
  case SDL_EVENT_GAMEPAD_REMAPPED:
    sync_gamepad(e.gdevice.which);
    return false;
  case SDL_EVENT_GAMEPAD_REMOVED:
    spo_input_reset(e.gdevice.which);
    return false;
  case SDL_EVENT_WINDOW_FOCUS_LOST:
    spo_input_reset(SPO_ALL_CONTROLLERS);
    return false;
  default:
    return false;
  }
}

void shutdown() {
  std::lock_guard lock(g_lock);
  if (!g_ready.exchange(false)) {
    return;
  }
  if (spo_config_dirty()) {
    spo_config_save();
  }
  spo_shutdown();
}
} // namespace aurora::starlet_backend::overlay
#else
namespace aurora::starlet_backend::overlay {
void frame(int, int) {}
bool event(const SDL_Event&) { return false; }
void shutdown() {}
} // namespace aurora::starlet_backend::overlay
#endif
