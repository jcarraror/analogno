#pragma once

#include "music_types.hpp"
#include "voice_sequencer.hpp"
#include "web_state.hpp"

#include <chrono>
#include <optional>
#include <vector>

namespace analogno {

struct SeqStep final {
  bool active{false};
  bool tie{false};
  int degree{0};
  int velocity{100};
  int midi_note{-1}; // -1 = compute from root/scale/octave; >=0 = absolute pitch
};

struct SeqTrack final {
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
  std::vector<SeqStep> steps = std::vector<SeqStep>(32);
  std::optional<Note> pending_note_off{};
};

struct Sequencer final {
  static constexpr int min_step_count = 8;
  static constexpr int max_step_count = 64;
  static constexpr int max_tracks = 16;

  bool playing{false};
  int active_track{0};
  int selected_step{-1}; // step in active_track armed for controller input; -1 = none
  float bpm{120.0F};
  int gate_pct{50};
  int step_count{32};
  int step_division{16};
  int playhead_step{-1};
  int current_step{-1};
  std::chrono::steady_clock::time_point step_start{};
  std::vector<SeqTrack> tracks = [] {
    std::vector<SeqTrack> v(4);
    for (int i = 0; i < 4; ++i) {
      v[static_cast<std::size_t>(i)].midi_channel = i;
    }
    return v;
  }();
};

struct SeqTick final {
  std::vector<Note> note_ons{};
  std::vector<Note> note_offs{};
  struct SampleEvent final {
    int bank{};
    Note note{};
  };
  std::vector<SampleEvent> sample_note_ons{};
  std::vector<SampleEvent> sample_note_offs{};
  bool stepped{false}; // true the frame a new step fires
};

[[nodiscard]] int effective_midi_channel(const SeqTrack &track);
[[nodiscard]] int sequencer_step_count(int value);
[[nodiscard]] int sequencer_step_division(int value);
[[nodiscard]] int track_loop_length(const SeqTrack &track, int step_count);
[[nodiscard]] int active_track_loop_length(const Sequencer &seq);
[[nodiscard]] std::optional<Note> note_for_step(const SeqStep &step,
                                                const SeqTrack &track,
                                                const MusicalIntent &ctx);
[[nodiscard]] SeqTick tick_sequencer(Sequencer &seq, const MusicalIntent &ctx);
[[nodiscard]] WebSeqState seq_web_state(const Sequencer &seq);

void resize_sequencer_steps(Sequencer &seq, int step_count);
void apply_voice_pattern_to_seq(Sequencer &seq,
                                const VoiceSequencerPattern &pattern);

} // namespace analogno
