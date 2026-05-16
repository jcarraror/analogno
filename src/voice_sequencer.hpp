#pragma once

#include "audio_capture.hpp"
#include "music_types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace analogno {

enum class VoiceSequencerMode : std::uint8_t {
  percussion,
  harmonic,
  hybrid,
};

struct VoiceSequencerConfig final {
  bool enabled{};
  VoiceSequencerMode mode{VoiceSequencerMode::percussion};
  bool snap_to_scale{true};
  float sensitivity{0.65F};
  float timing_offset_ms{};
};

struct VoiceSequencerStep final {
  bool active{};
  bool tie{};
  Note note{};
};

struct VoiceSequencerPattern final {
  std::vector<VoiceSequencerStep> steps{};
  std::size_t segment_count{};
};

struct VoiceSequencerStatus final {
  bool compiled{};
  bool available{};
  bool enabled{};
  bool recording{};
  VoiceSequencerMode mode{VoiceSequencerMode::percussion};
  bool snap_to_scale{};
  float sensitivity{};
  float timing_offset_ms{};
  int last_midi_note{-1};
  int last_velocity{};
  std::size_t accepted_notes{};
  std::size_t rejected_notes{};
  std::size_t recorded_segments{};
  float record_progress{};
};

class VoiceSequencer final {
public:
  VoiceSequencer();
  ~VoiceSequencer();

  VoiceSequencer(const VoiceSequencer &) = delete;
  VoiceSequencer &operator=(const VoiceSequencer &) = delete;

  VoiceSequencer(VoiceSequencer &&) = delete;
  VoiceSequencer &operator=(VoiceSequencer &&) = delete;

  void configure(const VoiceSequencerConfig &config);
  void start_recording(float bpm, int step_count);
  [[nodiscard]] std::optional<VoiceSequencerPattern> stop_recording(
      int root_midi_note, int octave_offset, ScaleKind scale);
  [[nodiscard]] const VoiceSequencerConfig &config() const;
  [[nodiscard]] VoiceSequencerStatus status() const;

  void process(AudioCapture &capture, int root_midi_note, int octave_offset,
               ScaleKind scale);

private:
  struct Segment final {
    Note note{};
    std::uint64_t start_frame{};
    std::uint64_t end_frame{};
  };

  struct TimedEvent final {
    Note note{};
    std::uint64_t frame{};
    bool pitched{};
  };

  VoiceSequencerConfig config_{};
  int last_midi_note_{-1};
  int last_velocity_{};
  std::size_t accepted_notes_{};
  std::size_t rejected_notes_{};
  std::uint64_t frame_cursor_{};
  bool recording_{};
  float record_bpm_{120.0F};
  int record_step_count_{16};
  std::uint64_t record_start_frame_{};
  std::vector<Segment> segments_{};
  std::vector<TimedEvent> events_{};
  std::optional<Segment> active_segment_{};

  struct Impl;
  Impl *impl_{};

  void add_event(const Note &note, std::uint64_t frame, bool pitched);
};

} // namespace analogno
