#pragma once

#include <cstdint>
#include <vector>

namespace analogno {

struct TranscribedNote {
  int step{};
  int duration_steps{1};
  int midi_note{};
  int velocity{100};
  std::size_t onset_frame{};  // frame within the trimmed region (0-based)
  int onset_idx{};            // index into TranscriptionResult::onset_frames
};

struct TranscriptionResult {
  std::vector<std::vector<TranscribedNote>> tracks{};  // [track_idx][note_idx]
  std::vector<double> onset_frames{};  // all onset positions, region-relative (frames)
  int suggested_root_note{48};
  int suggested_loop_length{0};
};

TranscriptionResult transcribe_to_seq(
    const std::vector<float> &samples,
    std::uint32_t channel_count,
    float trim_start,
    float trim_end,
    float bpm,
    int step_division,
    int step_count);

} // namespace analogno
