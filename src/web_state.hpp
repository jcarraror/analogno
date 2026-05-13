#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace analogno {

struct WebVec3 final {
  float x{};
  float y{};
  float z{};
};

struct WebControllerState final {
  float left_x{};
  float left_y{};
  float right_x{};
  float right_y{};
  float left_trigger{};
  float right_trigger{};
  bool has_gyro{};
  bool has_accel{};
  WebVec3 gyro{};
  WebVec3 accel{};
};

struct WebMusicState final {
  int root_midi_note{};
  int octave_offset{};
  std::string scale{};
  float pitch_bend{};
  float expression{};
  float filter_cutoff{};
  float filter_resonance{};
  float modulation{};
  float vibrato{};
  std::vector<int> active_notes{};
};

struct WebRuntimeState final {
  WebControllerState controller{};
  WebMusicState music{};
};

} // namespace analogno
