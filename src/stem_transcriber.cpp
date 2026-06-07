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

  constexpr uint_t sample_rate  = 48000U;
  constexpr uint_t hop_size     = 256U;
  constexpr uint_t onset_buf    = 512U;
  constexpr uint_t pitch_buf    = 2048U; // smaller than before; only called at onsets
  constexpr std::size_t min_gap = 1920U; // 40 ms

  const std::size_t region_frames = end_frame - start_frame;
  const int min_count = std::max(2, step_count / 4);

  // ------------------------------------------------------------------
  // Phase 1: onset detection only — no pitch computation per hop.
  // ------------------------------------------------------------------
  struct RawOnset { std::size_t frame; float peak; };
  std::vector<RawOnset> raw_onsets;

  {
    auto *onset = new_aubio_onset("complex", onset_buf, hop_size, sample_rate);
    if (!onset) return result;

    aubio_onset_set_silence(onset, -60.0F);
    aubio_onset_set_threshold(onset, 0.2F); // single moderate threshold

    auto *input     = new_fvec(hop_size);
    auto *onset_out = new_fvec(1);

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

      if (onset_out->data[0] > 0.0F) {
        const auto onset_sample = static_cast<std::size_t>(aubio_onset_get_last(onset));
        const auto onset_f = onset_sample < region_frames ? onset_sample : frame_pos;

        if (last_onset_frame != std::numeric_limits<std::size_t>::max() &&
            onset_f < last_onset_frame + min_gap) {
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

        raw_onsets.push_back({onset_f, peak});
      }
    }

    del_fvec(onset_out);
    del_fvec(input);
    del_aubio_onset(onset);
  }

  // ------------------------------------------------------------------
  // Grid-scan fallback: scan hop-by-hop (not frame-by-frame) for peaks.
  // Only runs if onset detection found too little.
  // ------------------------------------------------------------------
  if (static_cast<int>(raw_onsets.size()) < min_count) {
    float recording_peak = 0.0F;
    for (std::size_t frame_pos = 0; frame_pos + hop_size <= region_frames;
         frame_pos += hop_size) {
      for (uint_t i = 0; i < hop_size; ++i) {
        float s = 0.0F;
        for (std::uint32_t c = 0; c < ch; ++c)
          s += samples[(start_frame + frame_pos + i) * ch + c];
        recording_peak = std::max(recording_peak, std::abs(s / static_cast<float>(ch)));
      }
    }

    const float include_floor = recording_peak * 0.3F;
    if (recording_peak > 0.01F) {
      raw_onsets.clear();
      const double slice_dur =
          static_cast<double>(region_frames) / static_cast<double>(step_count);
      for (int s = 0; s < step_count; ++s) {
        const auto s_start = static_cast<std::size_t>(s * slice_dur);
        const auto s_end   = std::min(
            static_cast<std::size_t>((s + 1) * slice_dur), region_frames);

        float peak = 0.0F;
        std::size_t peak_frame = s_start;
        for (auto f = s_start; f < s_end; f += hop_size) {
          for (uint_t i = 0; i < hop_size && (f + i) < s_end; ++i) {
            float sv = 0.0F;
            for (std::uint32_t c = 0; c < ch; ++c)
              sv += samples[(start_frame + f + i) * ch + c];
            const float mono = std::abs(sv / static_cast<float>(ch));
            if (mono > peak) { peak = mono; peak_frame = f + i; }
          }
        }

        if (peak >= include_floor)
          raw_onsets.push_back({peak_frame, peak});
      }
    }
  }

  if (raw_onsets.empty()) return result;

  // ------------------------------------------------------------------
  // Phase 2: pitch detection — one call per onset, not per hop.
  // ------------------------------------------------------------------
  struct Detection { std::size_t onset_frame; int midi_note; int velocity; };
  std::vector<Detection> detections;
  detections.reserve(raw_onsets.size());

  {
    auto *pitch     = new_aubio_pitch("yin", pitch_buf, pitch_buf, sample_rate);
    auto *pitch_in  = new_fvec(pitch_buf);
    auto *pitch_out = new_fvec(1);

    if (pitch) {
      aubio_pitch_set_silence(pitch, -55.0F);
      aubio_pitch_set_tolerance(pitch, 0.15F);
    }

    for (const auto &ro : raw_onsets) {
      int midi = 48;

      if (pitch) {
        for (uint_t i = 0; i < pitch_buf; ++i) {
          const auto src = start_frame + ro.frame + i;
          float mono = 0.0F;
          if (src < end_frame) {
            for (std::uint32_t c = 0; c < ch; ++c) mono += samples[src * ch + c];
            mono /= static_cast<float>(ch);
          }
          pitch_in->data[i] = mono;
        }

        aubio_pitch_do(pitch, pitch_in, pitch_out);
        const auto hz   = pitch_out->data[0];
        const auto conf = aubio_pitch_get_confidence(pitch);
        if (conf > 0.5F && hz > 20.0F && hz < 8000.0F) {
          midi = std::clamp(
              static_cast<int>(std::round(69.0 + 12.0 * std::log2(
                  static_cast<double>(hz) / 440.0))),
              0, 127);
        }
      }

      const int vel = std::clamp(static_cast<int>(ro.peak * 127.0F), 1, 127);
      detections.push_back({ro.frame, midi, vel});
    }

    if (pitch_out) del_fvec(pitch_out);
    if (pitch_in)  del_fvec(pitch_in);
    if (pitch)     del_aubio_pitch(pitch);
  }

  if (detections.empty()) return result;

  std::sort(detections.begin(), detections.end(),
            [](const auto &a, const auto &b) {
              return a.onset_frame < b.onset_frame;
            });

  result.onset_frames.reserve(detections.size());
  for (const auto &d : detections)
    result.onset_frames.push_back(static_cast<double>(d.onset_frame));

  // ------------------------------------------------------------------
  // Build sequencer tracks.
  // ------------------------------------------------------------------
  const double step_dur_sec =
      60.0 / static_cast<double>(bpm) * 4.0 / static_cast<double>(step_division);

  // Normalise to the first onset so the pattern always starts at step 0
  // regardless of leading silence in the stem.
  const double first_onset_sec = static_cast<double>(detections.front().onset_frame)
                                  / static_cast<double>(sample_rate);

  struct StepEntry { int onset_idx; int velocity; };
  std::map<int, std::vector<StepEntry>> by_step;

  for (int i = 0; i < static_cast<int>(detections.size()); ++i) {
    const auto &d = detections[static_cast<std::size_t>(i)];
    const double sec = static_cast<double>(d.onset_frame) / static_cast<double>(sample_rate)
                       - first_onset_sec;
    const auto raw_step = static_cast<int>(std::round(sec / step_dur_sec));
    const auto step = ((raw_step % step_count) + step_count) % step_count;
    by_step[step].push_back({i, d.velocity});
  }

  std::size_t max_poly = 0;
  for (const auto &[step, notes] : by_step)
    max_poly = std::max(max_poly, notes.size());
  const auto track_count = std::min(max_poly, std::size_t{4});
  result.tracks.resize(track_count);

  for (const auto &[step, notes] : by_step) {
    auto sorted = notes;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto &a, const auto &b) { return a.velocity > b.velocity; });
    for (std::size_t t = 0; t < std::min(sorted.size(), track_count); ++t) {
      const auto &se  = sorted[t];
      const auto &det = detections[static_cast<std::size_t>(se.onset_idx)];
      result.tracks[t].push_back(TranscribedNote{
          .step        = step,
          .midi_note   = det.midi_note,
          .velocity    = det.velocity,
          .onset_frame = det.onset_frame,
          .onset_idx   = se.onset_idx,
      });
    }
  }

  for (auto &track : result.tracks)
    std::sort(track.begin(), track.end(),
              [](const auto &a, const auto &b) { return a.step < b.step; });

  // Discard tracks that have fewer than 2 notes.
  result.tracks.erase(
      std::remove_if(result.tracks.begin(), result.tracks.end(),
          [](const std::vector<TranscribedNote> &t) { return t.size() < 2; }),
      result.tracks.end());

  if (!result.tracks.empty() && !result.tracks[0].empty()) {
    result.suggested_root_note =
        std::min_element(result.tracks[0].begin(), result.tracks[0].end(),
                         [](const auto &a, const auto &b) {
                           return a.midi_note < b.midi_note;
                         })->midi_note;
  }

  const auto phrase_dur_sec =
      static_cast<double>(region_frames) / static_cast<double>(sample_rate);
  const auto raw_steps = static_cast<int>(std::ceil(phrase_dur_sec / step_dur_sec));
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
