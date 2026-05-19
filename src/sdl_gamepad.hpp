#pragma once

#include "sdl_check.hpp"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_sensor.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace analogno {

class Gamepad final {
public:
  explicit Gamepad(SDL_JoystickID id) : handle_{SDL_OpenGamepad(id)} {
    if (handle_ == nullptr) {
      fail_sdl("SDL_OpenGamepad failed");
    }
  }

  ~Gamepad() {
    if (handle_ != nullptr) {
      SDL_CloseGamepad(handle_);
    }
  }

  Gamepad(const Gamepad &) = delete;
  Gamepad &operator=(const Gamepad &) = delete;

  Gamepad(Gamepad &&other) noexcept : handle_{other.handle_} {
    other.handle_ = nullptr;
  }

  Gamepad &operator=(Gamepad &&other) noexcept {
    if (this == &other) {
      return *this;
    }

    if (handle_ != nullptr) {
      SDL_CloseGamepad(handle_);
    }

    handle_ = other.handle_;
    other.handle_ = nullptr;
    return *this;
  }

  [[nodiscard]] SDL_Gamepad *get() const { return handle_; }

  [[nodiscard]] const char *name() const {
    const char *value = SDL_GetGamepadName(handle_);
    return value != nullptr ? value : "unknown";
  }

  [[nodiscard]] SDL_JoystickID id() const {
    return SDL_GetGamepadID(handle_);
  }

  [[nodiscard]] SDL_GamepadType type() const {
    return SDL_GetGamepadType(handle_);
  }

  [[nodiscard]] int touchpad_count() const {
    return SDL_GetNumGamepadTouchpads(handle_);
  }

  [[nodiscard]] bool has_sensor(SDL_SensorType sensor) const {
    return SDL_GamepadHasSensor(handle_, sensor);
  }

  void enable_sensor(SDL_SensorType sensor, std::string_view name) const {
    if (!has_sensor(sensor)) {
      std::cout << "sensor unavailable: " << name << '\n';
      return;
    }

    if (!SDL_SetGamepadSensorEnabled(handle_, sensor, true)) {
      warn_sdl("SDL_SetGamepadSensorEnabled failed");
      return;
    }

    std::cout << "sensor enabled: " << name << '\n';
  }

  void set_led(std::uint8_t r, std::uint8_t g, std::uint8_t b) const {
    SDL_SetGamepadLED(handle_, r, g, b);
  }

  void rumble(std::uint16_t low_freq, std::uint16_t high_freq,
              std::uint32_t duration_ms) const {
    SDL_RumbleGamepad(handle_, low_freq, high_freq, duration_ms);
  }

private:
  SDL_Gamepad *handle_{nullptr};
};

inline std::string_view gamepad_type_name(SDL_GamepadType type) {
  switch (type) {
  case SDL_GAMEPAD_TYPE_STANDARD:
    return "standard";
  case SDL_GAMEPAD_TYPE_XBOX360:
    return "xbox360";
  case SDL_GAMEPAD_TYPE_XBOXONE:
    return "xboxone";
  case SDL_GAMEPAD_TYPE_PS3:
    return "ps3";
  case SDL_GAMEPAD_TYPE_PS4:
    return "ps4";
  case SDL_GAMEPAD_TYPE_PS5:
    return "ps5";
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    return "switch-pro";
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    return "switch-joycon-left";
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    return "switch-joycon-right";
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
    return "switch-joycon-pair";
  default:
    return "unknown";
  }
}

inline std::string_view sensor_name(SDL_SensorType sensor) {
  switch (sensor) {
  case SDL_SENSOR_ACCEL:
    return "accelerometer";
  case SDL_SENSOR_GYRO:
    return "gyro";
  default:
    return "unknown";
  }
}

inline const char *button_name(SDL_GamepadButton button) {
  const char *name = SDL_GetGamepadStringForButton(button);
  return name != nullptr ? name : "unknown";
}

inline const char *axis_name(SDL_GamepadAxis axis) {
  const char *name = SDL_GetGamepadStringForAxis(axis);
  return name != nullptr ? name : "unknown";
}

inline float normalized_axis(std::int16_t raw) {
  constexpr auto min_value = -32768.0F;
  constexpr auto max_value = 32767.0F;

  if (raw < 0) {
    return static_cast<float>(raw) / -min_value;
  }

  return static_cast<float>(raw) / max_value;
}

} // namespace analogno
