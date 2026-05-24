#include "stem_transcriber.hpp"

#if ANALOGNO_HAVE_AUBIO
#include <aubio/aubio.h>
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

namespace analogno {

TranscriptionResult transcribe_to_seq(
    const std::vector<float> &samples,
    std::uint32_t channel_count,
    float trim_start,
    float trim_end,
    float bpm,
    int step_division,
    int step_count) {

  TranscriptionResult result{};

#if !ANALOGNO_HAVE_AUBIO
  static_cast<void>(samples);
  static_cast<void>(channel_count);
  static_cast<void>(trim_start);
  static_cast<void>(trim_end);
  static_cast<void>(bpm);
  static_cast<void>(step_division);
  static_cast<void>(step_count);
  return result;
#else
  if (samples.empty() || bpm <= 0.0F || step_division <= 0 || step_count <= 0) {
    return result;
  }

  const auto ch = std::max(channel_count, 1U);
  const auto total_frames = samples.size() / ch;
  const auto start_frame = static_cast<std::size_t>(
      trim_start * static_cast<float>(total_frames));
  const auto end_frame = std::min(
      static_cast<std::size_t>(trim_end * static_cast<float>(total_frames)),
      total_frames);

  if (start_frame >= end_frame) return result;

  constexpr uint_t sample_rate = 48000U;
  constexpr uint_t hop_size = 256U;
  constexpr uint_t onset_buf = 512U;
  constexpr uint_t pitch_buf = 4096U;
  constexpr std::size_t min_onset_gap = 1920U;  // 40 ms

  auto *onset = new_aubio_onset("complex", onset_buf, hop_size, sample_rate);
  auto *pitch = new_aubio_pitch("yin", pitch_buf, hop_size, sample_rate);

  if (!onset || !pitch) {
    if (onset) del_aubio_onset(onset);
    if (pitch) del_aubio_pitch(pitch);
    return result;
  }

  aubio_onset_set_silence(onset, -60.0F);
  aubio_onset_set_threshold(onset, 0.3F);
  aubio_pitch_set_silence(pitch, -55.0F);
  aubio_pitch_set_tolerance(pitch, 0.15F);

  auto *input    = new_fvec(hop_size);
  auto *onset_out = new_fvec(1);
  auto *pitch_out = new_fvec(1);

  const double step_dur_sec =
      60.0 / static_cast<double>(bpm) * 4.0 / static_cast<double>(step_division);
  const std::size_t region_frames = end_frame - start_frame;

  struct Detection {
    std::size_t onset_frame;  // region-relative (0 = first frame of trim region)
    int midi_note;
    int velocity;
  };
  std::vector<Detection> detections;

  float last_pitch_hz = 0.0F;
  float last_confidence = 0.0F;
  std::size_t last_onset_frame = std::numeric_limits<std::size_t>::max();

  for (std::size_t frame_pos = 0; frame_pos + hop_size <= region_frames;
       frame_pos += hop_size) {
    for (uint_t i = 0; i < hop_size; ++i) {
      const auto src = start_frame + frame_pos + i;
      float mono = 0.0F;
      for (std::uint32_t c = 0; c < ch; ++c) mono += samples[src * ch + c];
      input->data[i] = mono / static_cast<float>(ch);
    }

    aubio_onset_do(onset, input, onset_out);
    aubio_pitch_do(pitch, input, pitch_out);
    last_pitch_hz   = pitch_out->data[0];
    last_confidence = aubio_pitch_get_confidence(pitch);

    if (onset_out->data[0] > 0.0F) {
      const auto onset_sample = static_cast<std::size_t>(aubio_onset_get_last(onset));
      const auto onset_f = onset_sample < region_frames ? onset_sample : frame_pos;

      if (last_onset_frame != std::numeric_limits<std::size_t>::max() &&
          onset_f < last_onset_frame + min_onset_gap) {
        continue;
      }
      last_onset_frame = onset_f;

      float peak = 0.0F;
      const auto vel_end = std::min(start_frame + onset_f + 512U, end_frame);
      for (auto f = start_frame + onset_f; f < vel_end; ++f) {
        float s = 0.0F;
        for (std::uint32_t c = 0; c < ch; ++c) s += samples[f * ch + c];
        peak = std::max(peak, std::abs(s / static_cast<float>(ch)));
      }

      int midi = 48;
      if (last_confidence > 0.5F && last_pitch_hz > 20.0F &&
          last_pitch_hz < 8000.0F) {
        const auto hz = static_cast<double>(last_pitch_hz);
        midi = std::clamp(
            static_cast<int>(std::round(69.0 + 12.0 * std::log2(hz / 440.0))),
            0, 127);
      }

      const auto vel = std::clamp(static_cast<int>(peak * 127.0F), 1, 127);
      detections.push_back({onset_f, midi, vel});
    }
  }

  del_fvec(pitch_out);
  del_fvec(onset_out);
  del_fvec(input);
  del_aubio_pitch(pitch);
  del_aubio_onset(onset);

  if (detections.empty()) return result;

  std::sort(detections.begin(), detections.end(),
            [](const auto &a, const auto &b) {
              return a.onset_frame < b.onset_frame;
            });

  result.onset_frames.reserve(detections.size());
  for (const auto &d : detections) {
    result.onset_frames.push_back(static_cast<double>(d.onset_frame));
  }

  struct StepEntry {
    int onset_idx;
    int velocity;
  };
  std::map<int, std::vector<StepEntry>> by_step;

  for (int i = 0; i < static_cast<int>(detections.size()); ++i) {
    const auto &d = detections[static_cast<std::size_t>(i)];
    const double sec =
        static_cast<double>(d.onset_frame) / static_cast<double>(sample_rate);
    const auto raw_step = static_cast<int>(std::round(sec / step_dur_sec));
    const auto step = ((raw_step % step_count) + step_count) % step_count;
    by_step[step].push_back({i, d.velocity});
  }

  std::size_t max_poly = 0;
  for (const auto &[step, notes] : by_step) {
    max_poly = std::max(max_poly, notes.size());
  }
  const auto track_count = std::min(max_poly, std::size_t{4});
  result.tracks.resize(track_count);

  for (const auto &[step, notes] : by_step) {
    auto sorted = notes;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto &a, const auto &b) {
                return a.velocity > b.velocity;
              });
    for (std::size_t t = 0; t < std::min(sorted.size(), track_count); ++t) {
      const auto &se = sorted[t];
      const auto &det = detections[static_cast<std::size_t>(se.onset_idx)];
      result.tracks[t].push_back(TranscribedNote{
          .step = step,
          .midi_note = det.midi_note,
          .velocity = det.velocity,
          .onset_frame = det.onset_frame,
          .onset_idx = se.onset_idx,
      });
    }
  }

  for (auto &track : result.tracks) {
    std::sort(track.begin(), track.end(),
              [](const auto &a, const auto &b) { return a.step < b.step; });
  }

  if (!result.tracks.empty() && !result.tracks[0].empty()) {
    result.suggested_root_note =
        std::min_element(result.tracks[0].begin(), result.tracks[0].end(),
                         [](const auto &a, const auto &b) {
                           return a.midi_note < b.midi_note;
                         })->midi_note;
  }

  const auto phrase_dur_sec =
      static_cast<double>(region_frames) / static_cast<double>(sample_rate);
  const auto raw_steps =
      static_cast<int>(std::ceil(phrase_dur_sec / step_dur_sec));
  int snapped = 8;
  for (const int boundary : {8, 16, 32, 64}) {
    snapped = boundary;
    if (raw_steps <= boundary) break;
  }
  result.suggested_loop_length = std::min(snapped, step_count);

  return result;
#endif
}

} // namespace analogno
