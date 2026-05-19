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
  float signals_volume{1.0F};
  bool blow_mode{};              // breath/wind controller mode active
  float wavetable_morph{};
  float wavetable_noise{};
  float wavetable_unison{};
  float blow_sensitivity{0.5F};  // 0..1 user-adjustable threshold
  bool blow_active{};            // note currently held by breath
  float blow_level{};            // 0..2: envelope relative to on_threshold (1.0 = at gate)
  bool voice_seq_available{};    // aubio-backed voice-to-sequencer is compiled and ready
  bool voice_seq_compiled{};      // app binary was built with aubio support
  bool voice_seq_enabled{};      // mic notes are being written into sequencer steps
  bool voice_seq_recording{};    // timed phrase capture is active
  std::string voice_seq_mode{"percussion"};
  bool voice_seq_snap{};         // snap detected notes to current scale before writing
  float voice_seq_sensitivity{0.65F};
  float voice_seq_timing_offset_ms{};
  int voice_seq_last_note{-1};
  int voice_seq_last_velocity{};
  std::size_t voice_seq_accepted_notes{};
  std::size_t voice_seq_rejected_notes{};
  std::size_t voice_seq_recorded_segments{};
  float voice_seq_record_progress{};
  std::vector<float> spec_samples{}; // 2048 raw audio samples for frontend FFT
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
  bool tie{};
  int degree{};
  int velocity{100};
  int midi_note{-1};
};

struct WebSeqTrack final {
  int midi_channel{0};
  int midi_program{0};
  int midi_bank{0};
  int sample_bank{-1};
  int loop_length{32};
  int volume{100};
  int pan{64};
  int velocity_scale{100};
  bool muted{false};
  bool solo{false};
  std::vector<WebSeqStep> steps{};
};

struct WebSeqState final {
  bool playing{};
  int active_track{0};
  int selected_step{-1};
  float bpm{120.0F};
  int playhead_step{-1};
  int current_step{-1};
  int gate_pct{50};
  int step_count{32};
  int step_division{16};
  std::vector<WebSeqTrack> tracks{};
};

struct WebRuntimeState final {
  WebControllerState controller{};
  WebMusicState music{};
  WebAudioState audio{};
  WebSeqState seq{};
  bool piano_roll_visible{true};
  bool spectrogram_visible{true};
};

struct WebLibraryState final {
  std::vector<WebPreset> presets{};
  std::vector<std::string> soundfonts{}; // available SF2 files on disk
  std::string active_soundfont{};        // currently browsed soundfont path
};

} // namespace analogno
