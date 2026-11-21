#include "overlay.hpp"
#include "backend.hpp"

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
#include "backend.hpp"

#include <deque>
#include <string>
#include <vector>

namespace aurora::starlet_backend::overlay {
namespace {
Module Log("aurora::starlet_backend::overlay");

std::mutex g_lock;
std::atomic<bool> g_ready{false};
bool g_failed = false;

struct QueuedOption {
  std::string page_key, page_label, key, label, help;
  int type, default_value;
  std::vector<std::string> choices;
  int (*on_change)(int, void*);
  void* user;
};
std::mutex g_queue_lock;
std::deque<QueuedOption> g_queue;
std::vector<std::pair<std::string, spo_id>> g_pages;

spo_result changed(spo_id, std::int32_t value, void* context) {
  const auto* option = static_cast<const QueuedOption*>(context);
  return option->on_change == nullptr || option->on_change(value, option->user) == 0 ? SPO_OK : SPO_REJECTED;
}

// Under g_lock, after spo_init and before the first spo_frame.
void register_options() {
  std::deque<QueuedOption> queued;
  {
    std::lock_guard queue_lock(g_queue_lock);
    queued.swap(g_queue);
  }
  // The overlay keeps the descriptor's context pointer for the life of the process.
  for (auto& item : queued) {
    auto* option = new QueuedOption(std::move(item));
    spo_id page = 0;
    for (const auto& [key, id] : g_pages) {
      if (key == option->page_key) {
        page = id;
      }
    }
    if (page == 0) {
      spo_page_desc description{sizeof description, option->page_key.c_str(), option->page_label.c_str()};
      if (spo_register_page(&description, &page) != SPO_OK) {
        Log.warn("overlay: page '{}' was refused (error {})", option->page_key, static_cast<int>(spo_last_error()));
        delete option;
        continue;
      }
      g_pages.emplace_back(option->page_key, page);
    }
    std::vector<const char*> choices;
    for (const auto& choice : option->choices) {
      choices.push_back(choice.c_str());
    }
    spo_option_desc description{};
    description.struct_size = sizeof description;
    description.page = page;
    description.type = option->type == AURORA_STARLET_OPTION_CHOICE ? SPO_CHOICE : SPO_TOGGLE;
    description.key = option->key.c_str();
    description.label = option->label.c_str();
    description.help = option->help.c_str();
    description.default_value = option->default_value;
    description.choices = choices.empty() ? nullptr : choices.data();
    description.choice_count = static_cast<std::uint32_t>(choices.size());
    description.on_change = changed;
    description.user = option;
    spo_id id = 0;
    if (spo_register_option(&description, &id) != SPO_OK) {
      Log.warn("overlay: option '{}.{}' was refused (error {})", option->page_key, option->key,
               static_cast<int>(spo_last_error()));
    }
  }
}

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
  register_options();
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

bool add_option(const aurora_starlet_option& option) {
  if (option.page_key == nullptr || option.key == nullptr || option.on_change == nullptr) {
    return false;
  }
  QueuedOption queued{option.page_key, option.page_label ? option.page_label : option.page_key, option.key,
                      option.label ? option.label : option.key, option.help ? option.help : "", option.type,
                      option.default_value, {}, option.on_change, option.user};
  for (unsigned i = 0; i < option.choice_count && option.choices != nullptr; ++i) {
    queued.choices.emplace_back(option.choices[i] ? option.choices[i] : "");
  }
  std::lock_guard queue_lock(g_queue_lock);
  g_queue.push_back(std::move(queued));
  return true;
}

bool event(const SDL_Event& e) {
  if (!g_ready.load()) {
    return false;
  }
  std::lock_guard lock(g_lock);
  // The second screen's window and touch events; the overlay keeps its own touches from the game.
  if (spo_handle_sdl3_screen_event(&e) != 0) {
    return true;
  }
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
bool add_option(const aurora_starlet_option&) { return false; }
} // namespace aurora::starlet_backend::overlay
#endif
