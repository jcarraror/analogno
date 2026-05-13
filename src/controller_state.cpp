#include "controller_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace analogno {
namespace {

constexpr auto stick_deadzone = 0.08F;

bool valid_axis(SDL_GamepadAxis axis) {
  return axis >= 0 && axis < SDL_GAMEPAD_AXIS_COUNT;
}

bool valid_button(SDL_GamepadButton button) {
  return button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT;
}

} // namespace

void ControllerState::handle_axis(const SDL_GamepadAxisEvent &event) {
  const auto current_axis = static_cast<SDL_GamepadAxis>(event.axis);

  if (!valid_axis(current_axis)) {
    return;
  }

  const auto raw = static_cast<std::int16_t>(event.value);

  const auto value = current_axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                             current_axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
                         ? normalize_trigger_value(raw)
                         : normalize_axis_value(raw);

  const auto index = static_cast<std::size_t>(current_axis);

  if (axes_[index] != value) {
    axes_[index] = value;
    changed_this_frame_ = true;
  }
}

void ControllerState::handle_button_down(const SDL_GamepadButtonEvent &event) {
  const auto current_button = static_cast<SDL_GamepadButton>(event.button);

  if (!valid_button(current_button)) {
    return;
  }

  const auto index = static_cast<std::size_t>(current_button);

  if (!buttons_[index]) {
    pressed_[index] = true;
    changed_this_frame_ = true;
  }

  buttons_[index] = true;
}

void ControllerState::handle_button_up(const SDL_GamepadButtonEvent &event) {
  const auto current_button = static_cast<SDL_GamepadButton>(event.button);

  if (!valid_button(current_button)) {
    return;
  }

  const auto index = static_cast<std::size_t>(current_button);

  if (buttons_[index]) {
    released_[index] = true;
    changed_this_frame_ = true;
  }

  buttons_[index] = false;
}

void ControllerState::handle_sensor(const SDL_GamepadSensorEvent &event) {
  const Vec3 value{
      .x = event.data[0],
      .y = event.data[1],
      .z = event.data[2],
  };

  const auto sensor = static_cast<SDL_SensorType>(event.sensor);

  switch (sensor) {
  case SDL_SENSOR_GYRO:
    gyro_ = value;
    has_gyro_ = true;
    changed_this_frame_ = true;
    break;

  case SDL_SENSOR_ACCEL:
    accel_ = value;
    has_accel_ = true;
    changed_this_frame_ = true;
    break;

  default:
    break;
  }
}

void ControllerState::handle_touchpad_down(
    const SDL_GamepadTouchpadEvent &event) {
  handle_touchpad_motion(event);
}

void ControllerState::handle_touchpad_motion(
    const SDL_GamepadTouchpadEvent &event) {
  const auto touchpad = static_cast<std::size_t>(event.touchpad);
  const auto finger = static_cast<std::size_t>(event.finger);

  if (touchpad >= max_touchpads || finger >= max_touch_fingers) {
    return;
  }

  touch_[touchpad][finger] = TouchFinger{
      .down = true,
      .x = event.x,
      .y = event.y,
      .pressure = event.pressure,
  };

  changed_this_frame_ = true;
}

void ControllerState::handle_touchpad_up(
    const SDL_GamepadTouchpadEvent &event) {
  const auto touchpad = static_cast<std::size_t>(event.touchpad);
  const auto finger = static_cast<std::size_t>(event.finger);

  if (touchpad >= max_touchpads || finger >= max_touch_fingers) {
    return;
  }

  touch_[touchpad][finger].down = false;
  touch_[touchpad][finger].pressure = 0.0F;

  changed_this_frame_ = true;
}

float ControllerState::axis(SDL_GamepadAxis axis) const {
  if (!valid_axis(axis)) {
    return 0.0F;
  }

  return axes_[static_cast<std::size_t>(axis)];
}

bool ControllerState::button(SDL_GamepadButton button) const {
  if (!valid_button(button)) {
    return false;
  }

  return buttons_[static_cast<std::size_t>(button)];
}

bool ControllerState::button_pressed(SDL_GamepadButton button) const {
  if (!valid_button(button)) {
    return false;
  }

  return pressed_[static_cast<std::size_t>(button)];
}

bool ControllerState::button_released(SDL_GamepadButton button) const {
  if (!valid_button(button)) {
    return false;
  }

  return released_[static_cast<std::size_t>(button)];
}

float ControllerState::left_x() const {
  return axis(SDL_GAMEPAD_AXIS_LEFTX);
}

float ControllerState::left_y() const {
  return axis(SDL_GAMEPAD_AXIS_LEFTY);
}

float ControllerState::right_x() const {
  return axis(SDL_GAMEPAD_AXIS_RIGHTX);
}

float ControllerState::right_y() const {
  return axis(SDL_GAMEPAD_AXIS_RIGHTY);
}

float ControllerState::left_trigger() const {
  return axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
}

float ControllerState::right_trigger() const {
  return axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
}

Vec3 ControllerState::gyro() const { return gyro_; }

Vec3 ControllerState::accel() const { return accel_; }

bool ControllerState::has_gyro() const { return has_gyro_; }

bool ControllerState::has_accel() const { return has_accel_; }

TouchFinger ControllerState::touch_finger(std::size_t touchpad,
                                          std::size_t finger) const {
  if (touchpad >= max_touchpads || finger >= max_touch_fingers) {
    return {};
  }

  return touch_[touchpad][finger];
}

void ControllerState::clear_frame_edges() {
  pressed_.fill(false);
  released_.fill(false);
  changed_this_frame_ = false;
}

bool ControllerState::changed_this_frame() const {
  return changed_this_frame_;
}

float ControllerState::normalize_axis_value(std::int16_t raw) {
  constexpr auto negative_max = 32768.0F;
  constexpr auto positive_max = 32767.0F;

  const auto value = raw < 0 ? static_cast<float>(raw) / negative_max
                             : static_cast<float>(raw) / positive_max;

  return apply_deadzone(std::clamp(value, -1.0F, 1.0F));
}

float ControllerState::normalize_trigger_value(std::int16_t raw) {
  constexpr auto positive_max = 32767.0F;

  const auto value = static_cast<float>(raw) / positive_max;
  return std::clamp(value, 0.0F, 1.0F);
}

float ControllerState::apply_deadzone(float value) {
  if (std::abs(value) < stick_deadzone) {
    return 0.0F;
  }

  return value;
}

std::string_view axis_label(SDL_GamepadAxis axis) {
  switch (axis) {
  case SDL_GAMEPAD_AXIS_LEFTX:
    return "left_x";
  case SDL_GAMEPAD_AXIS_LEFTY:
    return "left_y";
  case SDL_GAMEPAD_AXIS_RIGHTX:
    return "right_x";
  case SDL_GAMEPAD_AXIS_RIGHTY:
    return "right_y";
  case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
    return "left_trigger";
  case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
    return "right_trigger";
  default:
    return "unknown";
  }
}

std::string_view button_label(SDL_GamepadButton button) {
  switch (button) {
  case SDL_GAMEPAD_BUTTON_SOUTH:
    return "south";
  case SDL_GAMEPAD_BUTTON_EAST:
    return "east";
  case SDL_GAMEPAD_BUTTON_WEST:
    return "west";
  case SDL_GAMEPAD_BUTTON_NORTH:
    return "north";
  case SDL_GAMEPAD_BUTTON_BACK:
    return "back";
  case SDL_GAMEPAD_BUTTON_GUIDE:
    return "guide";
  case SDL_GAMEPAD_BUTTON_START:
    return "start";
  case SDL_GAMEPAD_BUTTON_LEFT_STICK:
    return "left_stick";
  case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
    return "right_stick";
  case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
    return "left_shoulder";
  case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
    return "right_shoulder";
  case SDL_GAMEPAD_BUTTON_DPAD_UP:
    return "dpad_up";
  case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
    return "dpad_down";
  case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
    return "dpad_left";
  case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
    return "dpad_right";
  case SDL_GAMEPAD_BUTTON_MISC1:
    return "misc1";
  case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1:
    return "right_paddle_1";
  case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1:
    return "left_paddle_1";
  case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2:
    return "right_paddle_2";
  case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2:
    return "left_paddle_2";
  case SDL_GAMEPAD_BUTTON_TOUCHPAD:
    return "touchpad";
  default:
    return "unknown";
  }
}

} // namespace analogno
