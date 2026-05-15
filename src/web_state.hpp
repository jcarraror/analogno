#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace analogno {

struct WebPreset final {
  int bank{};
  int program{};
  std::string name{};
};

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

struct WebSampleBank final {
  bool has_sample{};
  std::uint32_t frames{};
  float trim_start{};
  float trim_end{1.0F};
  bool is_wavetable{};
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
  std::vector<float> sample_waveform{}; // downsampled peak-abs of captured sample
  std::vector<WebSampleBank> banks{};
  std::size_t active_bank{};
  std::vector<float> touchpad_sketch{};                   // live waveform preview while drawing
  bool touchpad_drawing{false};
  std::vector<std::array<float, 2>> touchpad_raw_points{}; // raw finger path [[x,y],...]
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
  int midi_program{};
  int midi_bank{};
  std::vector<int> button_midi_notes{}; // 8 entries: face(0-3) + dpad(4-7)
};

struct WebSeqStep final {
  bool active{};
  int degree{};
  int velocity{100};
  int midi_note{-1};
};

struct WebSeqTrack final {
  int midi_channel{0};
  int midi_program{0};
  int midi_bank{0};
  bool muted{false};
  std::vector<WebSeqStep> steps{};
};

struct WebSeqState final {
  bool playing{};
  int active_track{0};
  int selected_step{-1};
  float bpm{120.0F};
  int current_step{-1};
  int gate_pct{50};
  std::vector<WebSeqTrack> tracks{};
};

struct WebRuntimeState final {
  WebControllerState controller{};
  WebMusicState music{};
  WebAudioState audio{};
  WebSeqState seq{};
  std::vector<WebPreset> presets{};
  std::vector<std::string> soundfonts{};   // available SF2 files on disk
  std::string active_soundfont{};          // currently browsed soundfont path
  bool piano_roll_visible{true};
};

} // namespace analogno
