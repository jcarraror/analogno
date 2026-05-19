#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace analogno {

struct RumbleStep {
  std::uint16_t low{};
  std::uint16_t high{};
  std::uint32_t ms{};
  std::chrono::steady_clock::time_point fire_at{};
};

class DrumPad {
public:
  static constexpr int   page_count   = 4;
  static constexpr float nav_strip_y  = 0.12F;

  int page{0};
  std::array<int, 2> finger_note{-1, -1};
  std::chrono::steady_clock::time_point flash_until{};
  std::deque<RumbleStep> pending_rumble{};

  [[nodiscard]] bool in_nav_strip(float y) const noexcept {
    return y < nav_strip_y;
  }

  [[nodiscard]] int note_for_xy(float x, float y) const noexcept {
    constexpr std::array<std::array<std::array<int, 3>, 2>, page_count> pages = {{
      {{ {38, 42, 49}, {36, 45, 51} }},  // 0 Standard:  Snare|HH|Crash       / Kick|Tom|Ride
      {{ {46, 39, 55}, {37, 50, 53} }},  // 1 Extended:  OpenHH|Clap|Splash   / Rim|HiTom|RideBell
      {{ {56, 63, 60}, {79, 64, 61} }},  // 2 Latin:     Cowbell|CongaHi|BongoHi / Cuica|CongaLo|BongoLo
      {{ {71, 73, 81}, {54, 69, 82} }},  // 3 FX/Perc:   Whistle|Guiro|Triangle / Tambourine|Cabasa|Shaker
    }};
    const auto col = x < (1.0F / 3.0F) ? std::size_t{0}
                   : x < (2.0F / 3.0F) ? std::size_t{1} : std::size_t{2};
    const float drum_y = (y - nav_strip_y) / (1.0F - nav_strip_y);
    const auto row = drum_y < 0.5F ? std::size_t{0} : std::size_t{1};
    return pages[static_cast<std::size_t>(page)][row][col];
  }

  [[nodiscard]] std::array<std::uint8_t, 3> page_color() const noexcept {
    constexpr std::array<std::array<std::uint8_t, 3>, page_count> colors = {{
      {220, 220, 220},  // 0 Standard: white
      {  0, 200, 255},  // 1 Extended: cyan
      {255, 120,   0},  // 2 Latin:    orange
      {180,   0, 255},  // 3 FX/Perc:  violet
    }};
    return colors[static_cast<std::size_t>(page)];
  }

  void navigate(bool forward) noexcept {
    page = (page + (forward ? 1 : page_count - 1)) % page_count;
    const auto now = std::chrono::steady_clock::now();
    pending_rumble.clear();
    if (forward) {
      
      pending_rumble.push_back({2500, 0,    45, now});
      pending_rumble.push_back({0,    5000, 35, now + std::chrono::milliseconds{55}});
      pending_rumble.push_back({0,    8500, 25, now + std::chrono::milliseconds{100}});
    } else {
      
      pending_rumble.push_back({0,    8500, 25, now});
      pending_rumble.push_back({0,    5000, 35, now + std::chrono::milliseconds{35}});
      pending_rumble.push_back({2500, 0,    45, now + std::chrono::milliseconds{80}});
    }
    flash_until = now + std::chrono::milliseconds{350};
  }
};

} // namespace analogno
