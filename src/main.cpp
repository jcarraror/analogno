#include "sdl_check.hpp"
#include "sdl_gamepad.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_timer.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <thread>

namespace {

using analogno::Gamepad;

struct Sdl final {
  Sdl()
  {
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
      analogno::fail_sdl("SDL_Init failed");
    }
  }

  ~Sdl()
  {
    SDL_Quit();
  }

  Sdl(const Sdl&) = delete;
  auto operator=(const Sdl&) -> Sdl& = delete;
  Sdl(Sdl&&) = delete;
  auto operator=(Sdl&&) -> Sdl& = delete;
};

auto open_first_gamepad() -> std::optional<Gamepad>
{
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);

  if (ids == nullptr) {
    analogno::fail_sdl("SDL_GetGamepads failed");
  }

  std::unique_ptr<SDL_JoystickID[], decltype(&SDL_free)> ids_owner{
    ids,
    SDL_free
  };

  if (count == 0) {
    std::cout << "no gamepads found\n";
    return std::nullopt;
  }

  std::cout << "gamepads found: " << count << '\n';

  const std::span<const SDL_JoystickID> gamepad_ids{ids, static_cast<std::size_t>(count)};

  for (const SDL_JoystickID id : gamepad_ids) {
    const char* name = SDL_GetGamepadNameForID(id);
    std::cout << "  id=" << id << " name=" << (name != nullptr ? name : "unknown") << '\n';
  }

  return Gamepad{gamepad_ids.front()};
}

auto print_capabilities(const Gamepad& gamepad) -> void
{
  std::cout << '\n';
  std::cout << "opened gamepad\n";
  std::cout << "  id: " << gamepad.id() << '\n';
  std::cout << "  name: " << gamepad.name() << '\n';
  std::cout << "  type: " << analogno::gamepad_type_name(gamepad.type()) << '\n';
  std::cout << "  touchpads: " << gamepad.touchpad_count() << '\n';
  std::cout << "  has gyro: " << std::boolalpha << gamepad.has_sensor(SDL_SENSOR_GYRO) << '\n';
  std::cout << "  has accelerometer: " << std::boolalpha << gamepad.has_sensor(SDL_SENSOR_ACCEL) << '\n';
  std::cout << '\n';
}

auto enable_motion_sensors(const Gamepad& gamepad) -> void
{
  gamepad.enable_sensor(SDL_SENSOR_GYRO, "gyro");
  gamepad.enable_sensor(SDL_SENSOR_ACCEL, "accelerometer");
  std::cout << '\n';
}

auto handle_button_event(const SDL_GamepadButtonEvent& event, std::string_view state) -> void
{
  const auto button = static_cast<SDL_GamepadButton>(event.button);

  std::cout
    << "button "
    << state
    << " id=" << event.which
    << " button=" << analogno::button_name(button)
    << '\n';
}

auto handle_axis_event(const SDL_GamepadAxisEvent& event) -> void
{
  const auto axis = static_cast<SDL_GamepadAxis>(event.axis);
  const auto raw = static_cast<std::int16_t>(event.value);
  const auto normalized = analogno::normalized_axis(raw);

  std::cout
    << "axis"
    << " id=" << event.which
    << " axis=" << analogno::axis_name(axis)
    << " raw=" << raw
    << " norm=" << normalized
    << '\n';
}

auto handle_sensor_event(const SDL_GamepadSensorEvent& event) -> void
{
  const auto sensor = static_cast<SDL_SensorType>(event.sensor);

  std::cout
    << "sensor"
    << " id=" << event.which
    << " type=" << analogno::sensor_name(sensor)
    << " x=" << event.data[0]
    << " y=" << event.data[1]
    << " z=" << event.data[2]
    << '\n';
}

auto handle_touchpad_event(const SDL_GamepadTouchpadEvent& event, std::string_view state) -> void
{
  std::cout
    << "touchpad "
    << state
    << " id=" << event.which
    << " touchpad=" << event.touchpad
    << " finger=" << event.finger
    << " x=" << event.x
    << " y=" << event.y
    << " pressure=" << event.pressure
    << '\n';
}

auto run_event_loop() -> void
{
  std::cout << "event monitor running. press Ctrl+C or close the window to quit.\n\n";

  bool running = true;

  while (running) {
    SDL_Event event{};

    while (SDL_PollEvent(&event)) {
      switch (event.type) {
        case SDL_EVENT_QUIT:
          running = false;
          break;

        case SDL_EVENT_GAMEPAD_ADDED:
          std::cout << "gamepad added id=" << event.gdevice.which << '\n';
          break;

        case SDL_EVENT_GAMEPAD_REMOVED:
          std::cout << "gamepad removed id=" << event.gdevice.which << '\n';
          break;

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
          handle_button_event(event.gbutton, "down");
          break;

        case SDL_EVENT_GAMEPAD_BUTTON_UP:
          handle_button_event(event.gbutton, "up");
          break;

        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
          handle_axis_event(event.gaxis);
          break;

        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
          handle_sensor_event(event.gsensor);
          break;

        case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
          handle_touchpad_event(event.gtouchpad, "down");
          break;

        case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
          handle_touchpad_event(event.gtouchpad, "motion");
          break;

        case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
          handle_touchpad_event(event.gtouchpad, "up");
          break;

        default:
          break;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

} // namespace

auto main(int, char**) -> int
{
  const Sdl sdl{};

  auto gamepad = open_first_gamepad();

  if (!gamepad.has_value()) {
    return EXIT_FAILURE;
  }

  print_capabilities(*gamepad);
  enable_motion_sensors(*gamepad);
  run_event_loop();

  return EXIT_SUCCESS;
}
