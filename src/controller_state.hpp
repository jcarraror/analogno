#pragma once

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_sensor.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace analogno {

struct Vec3 final {
  float x{};
  float y{};
  float z{};
};

struct TouchFinger final {
  bool down{};
  float x{};
  float y{};
  float pressure{};
};

class ControllerState final {
public:
  static constexpr auto axis_count =
      static_cast<std::size_t>(SDL_GAMEPAD_AXIS_COUNT);
  static constexpr auto button_count =
      static_cast<std::size_t>(SDL_GAMEPAD_BUTTON_COUNT);
  static constexpr auto max_touchpads = std::size_t{2};
  static constexpr auto max_touch_fingers = std::size_t{4};

  void handle_axis(const SDL_GamepadAxisEvent &event);
  void handle_button_down(const SDL_GamepadButtonEvent &event);
  void handle_button_up(const SDL_GamepadButtonEvent &event);
  void handle_sensor(const SDL_GamepadSensorEvent &event);
  void handle_touchpad_down(const SDL_GamepadTouchpadEvent &event);
  void handle_touchpad_motion(const SDL_GamepadTouchpadEvent &event);
  void handle_touchpad_up(const SDL_GamepadTouchpadEvent &event);

  [[nodiscard]] float axis(SDL_GamepadAxis axis) const;
  [[nodiscard]] bool button(SDL_GamepadButton button) const;
  [[nodiscard]] bool button_pressed(SDL_GamepadButton button) const;
  [[nodiscard]] bool button_released(SDL_GamepadButton button) const;

  [[nodiscard]] float left_x() const;
  [[nodiscard]] float left_y() const;
  [[nodiscard]] float right_x() const;
  [[nodiscard]] float right_y() const;
  [[nodiscard]] float left_trigger() const;
  [[nodiscard]] float right_trigger() const;

  [[nodiscard]] Vec3 gyro() const;
  [[nodiscard]] Vec3 accel() const;

  [[nodiscard]] bool has_gyro() const;
  [[nodiscard]] bool has_accel() const;

  [[nodiscard]] TouchFinger touch_finger(std::size_t touchpad,
                                         std::size_t finger) const;

  void clear_frame_edges();

  [[nodiscard]] bool changed_this_frame() const;

private:
  std::array<float, axis_count> axes_{};
  std::array<bool, button_count> buttons_{};
  std::array<bool, button_count> pressed_{};
  std::array<bool, button_count> released_{};

  std::array<std::array<TouchFinger, max_touch_fingers>, max_touchpads>
      touch_{};

  Vec3 gyro_{};
  Vec3 accel_{};

  bool has_gyro_{};
  bool has_accel_{};
  bool changed_this_frame_{};

  static float normalize_axis_value(std::int16_t raw);
  static float normalize_trigger_value(std::int16_t raw);
  static float apply_deadzone(float value);
};

std::string_view axis_label(SDL_GamepadAxis axis);
std::string_view button_label(SDL_GamepadButton button);

} // namespace analogno
