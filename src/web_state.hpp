#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace analogno {

struct WebVec3 final {
  float x{};
  float y{};
  float z{};
};

struct WebCaptureDevice final {
  std::uint32_t index{};
  std::string name{};
  bool is_default{};
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

struct WebAudioState final {
  std::vector<WebCaptureDevice> devices{};
  std::optional<std::uint32_t> selected_device_index{};
  bool capture_running{};
  bool sample_recording{};
  std::string capture_device{};
  float mic_level{};
  float envelope{};
  bool gate_open{};
  bool onset{};
  int velocity{};
  std::vector<float> waveform{};
  bool sample_ready{};
  std::uint32_t sample_frames{};
  float sample_trim_start{};
  float sample_trim_end{1.0F};
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
  WebAudioState audio{};
};

} // namespace analogno
