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

  auto handle_axis(const SDL_GamepadAxisEvent &event) -> void;
  auto handle_button_down(const SDL_GamepadButtonEvent &event) -> void;
  auto handle_button_up(const SDL_GamepadButtonEvent &event) -> void;
  auto handle_sensor(const SDL_GamepadSensorEvent &event) -> void;
  auto handle_touchpad_down(const SDL_GamepadTouchpadEvent &event) -> void;
  auto handle_touchpad_motion(const SDL_GamepadTouchpadEvent &event) -> void;
  auto handle_touchpad_up(const SDL_GamepadTouchpadEvent &event) -> void;

  [[nodiscard]] auto axis(SDL_GamepadAxis axis) const -> float;
  [[nodiscard]] auto button(SDL_GamepadButton button) const -> bool;

  [[nodiscard]] auto left_x() const -> float;
  [[nodiscard]] auto left_y() const -> float;
  [[nodiscard]] auto right_x() const -> float;
  [[nodiscard]] auto right_y() const -> float;
  [[nodiscard]] auto left_trigger() const -> float;
  [[nodiscard]] auto right_trigger() const -> float;

  [[nodiscard]] auto gyro() const -> Vec3;
  [[nodiscard]] auto accel() const -> Vec3;

  [[nodiscard]] auto has_gyro() const -> bool;
  [[nodiscard]] auto has_accel() const -> bool;

  [[nodiscard]] auto touch_finger(std::size_t touchpad,
                                  std::size_t finger) const -> TouchFinger;

  auto clear_frame_edges() -> void;

  [[nodiscard]] auto changed_this_frame() const -> bool;

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

  static auto normalize_axis_value(std::int16_t raw) -> float;
  static auto normalize_trigger_value(std::int16_t raw) -> float;
  static auto apply_deadzone(float value) -> float;
};

auto axis_label(SDL_GamepadAxis axis) -> std::string_view;
auto button_label(SDL_GamepadButton button) -> std::string_view;

} // namespace analogno
