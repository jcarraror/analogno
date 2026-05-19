#pragma once

#include "music_types.hpp"

#include <algorithm>
#include <chrono>
#include <optional>

namespace analogno {

struct BlowController {
  bool enabled{false};
  bool is_blowing{false};
  std::optional<Note> held_note{};
  float sensitivity{0.5F};

  std::optional<std::chrono::steady_clock::time_point> attack_since{};
  std::optional<std::chrono::steady_clock::time_point> release_since{};
  std::chrono::steady_clock::time_point arm_until{};
  float ambient_floor{0.0F};
  bool ambient_ready{false};

  static constexpr auto attack_time      = std::chrono::milliseconds{45};
  static constexpr auto release_time     = std::chrono::milliseconds{90};
  static constexpr auto arm_cooldown_time = std::chrono::milliseconds{700};

  [[nodiscard]] float signal_threshold() const {
    const auto inverse = 1.0F - sensitivity;
    return 0.00035F + inverse * inverse * 0.00565F;
  }
  [[nodiscard]] float signal_level(float raw) const {
    return std::max(0.0F, raw - ambient_floor);
  }
  [[nodiscard]] float on_threshold() const {
    return ambient_floor + signal_threshold();
  }
  [[nodiscard]] float off_threshold() const {
    return ambient_floor + signal_threshold() * 0.35F;
  }
};

} // namespace analogno
