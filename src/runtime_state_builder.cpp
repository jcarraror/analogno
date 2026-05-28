#include "runtime_state_builder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>

namespace analogno {
namespace {

std::string_view voice_mode_name(VoiceSequencerMode mode) {
  switch (mode) {
  case VoiceSequencerMode::harmonic:
    return "harmonic";
  case VoiceSequencerMode::hybrid:
    return "hybrid";
  case VoiceSequencerMode::percussion:
    return "percussion";
  }

  return "percussion";
}

} // namespace

std::vector<float>
build_wavetable(const std::vector<std::pair<float, float>> &points,
                std::size_t n) {
  if (points.size() < 2 || n == 0) {
    return {};
  }

  constexpr auto canvas_bins = std::size_t{2048};

  std::vector<float> y_bins(canvas_bins, 0.0F);
  std::vector<bool> filled(canvas_bins, false);

  auto put_point = [&](float x, float y) {
    x = std::clamp(x, 0.0F, 1.0F);
    y = std::clamp(y, 0.0F, 1.0F);

    const auto bin = static_cast<std::size_t>(
        std::round(x * static_cast<float>(canvas_bins - 1)));

    y_bins[bin] = y;
    filled[bin] = true;
  };

  auto draw_segment = [&](float x0, float y0, float x1, float y1) {
    x0 = std::clamp(x0, 0.0F, 1.0F);
    y0 = std::clamp(y0, 0.0F, 1.0F);
    x1 = std::clamp(x1, 0.0F, 1.0F);
    y1 = std::clamp(y1, 0.0F, 1.0F);

    const auto b0 = static_cast<int>(
        std::round(x0 * static_cast<float>(canvas_bins - 1)));
    const auto b1 = static_cast<int>(
        std::round(x1 * static_cast<float>(canvas_bins - 1)));

    const auto steps = std::max(std::abs(b1 - b0), 1);

    for (int s = 0; s <= steps; ++s) {
      const float t = static_cast<float>(s) / static_cast<float>(steps);
      const float x = x0 + t * (x1 - x0);
      const float y = y0 + t * (y1 - y0);
      put_point(x, y);
    }
  };

  put_point(points.front().first, points.front().second);

  for (std::size_t i = 1; i < points.size(); ++i) {
    const auto [x0, y0] = points[i - 1];
    const auto [x1, y1] = points[i];

    if (x1 + 0.02F < x0) {
      continue;
    }

    draw_segment(x0, y0, x1, y1);
  }

  auto first = std::size_t{0};
  while (first < canvas_bins && !filled[first]) {
    ++first;
  }

  if (first == canvas_bins) {
    return {};
  }

  for (std::size_t i = 0; i < first; ++i) {
    y_bins[i] = y_bins[first];
    filled[i] = true;
  }

  std::size_t last = first;

  for (std::size_t i = first + 1; i < canvas_bins; ++i) {
    if (!filled[i]) {
      continue;
    }

    const auto next = i;

    if (next > last + 1) {
      const float y0 = y_bins[last];
      const float y1 = y_bins[next];

      for (std::size_t j = last + 1; j < next; ++j) {
        const float t = static_cast<float>(j - last) /
                        static_cast<float>(next - last);
        y_bins[j] = y0 + t * (y1 - y0);
        filled[j] = true;
      }
    }

    last = next;
  }

  for (std::size_t i = last + 1; i < canvas_bins; ++i) {
    y_bins[i] = y_bins[last];
    filled[i] = true;
  }

  std::vector<float> result(n);

  for (std::size_t i = 0; i < n; ++i) {
    const float src = static_cast<float>(i) *
                      static_cast<float>(canvas_bins - 1) /
                      static_cast<float>(n - 1);

    const auto lo = static_cast<std::size_t>(src);
    const auto hi = std::min(lo + 1, canvas_bins - 1);
    const float t = src - static_cast<float>(lo);

    const float y = y_bins[lo] + t * (y_bins[hi] - y_bins[lo]);

    result[i] = 1.0F - 2.0F * y;
  }

  const float mean =
      std::accumulate(result.begin(), result.end(), 0.0F) /
      static_cast<float>(result.size());

  for (float &v : result) {
    v -= mean;
  }

  const float max_abs = std::abs(*std::max_element(
      result.begin(), result.end(),
      [](float a, float b) { return std::abs(a) < std::abs(b); }));

  if (max_abs < 0.01F) {
    return {};
  }

  for (float &v : result) {
    v /= max_abs;
  }

  return result;
}

WebRuntimeState make_web_state(
    const ControllerState &controller, const MusicalIntent &intent,
    const std::vector<int> &active_notes, const AudioCapture &audio_capture,
    const AudioSampler &audio_sampler, const AudioFeatures &audio_features,
    int midi_program, int midi_bank, const WaveformSketch &sketch,
    const Sequencer &seq, bool piano_roll_visible, bool spectrogram_visible,
    bool blow_mode, float wavetable_morph, float wavetable_noise,
    float wavetable_unison, float blow_sensitivity, bool blow_active,
    float blow_level, const VoiceSequencerStatus &voice_seq_status,
    const std::vector<float> &spec_samples, float signals_volume,
    const std::string &stem_split_state, const std::string &stem_split_error,
    float stem_split_progress, const std::string &stem_split_detail,
    const std::vector<std::string> &stem_split_log,
    const std::string &stems_folder,
    const std::string &transcribe_state,
    bool mic_has_sample,
    const std::array<bool, AudioSampler::bank_count> &transcribe_cached) {
  std::vector<WebCaptureDevice> capture_devices{};

  for (const auto &device : audio_capture.devices()) {
    capture_devices.push_back(WebCaptureDevice{
        .index = device.index,
        .name = device.name,
        .is_default = device.is_default,
    });
  }

  std::vector<WebSampleBank> sample_banks{};
  sample_banks.reserve(AudioSampler::bank_count);

  for (std::size_t i = 0; i < AudioSampler::bank_count; ++i) {
    sample_banks.push_back(WebSampleBank{
        .has_sample = audio_sampler.bank_has_sample(i),
        .frames = static_cast<std::uint32_t>(audio_sampler.bank_frames(i)),
        .trim_start = audio_sampler.bank_trim_start(i),
        .trim_end = audio_sampler.bank_trim_end(i),
        .is_wavetable = audio_sampler.bank_is_wavetable(i),
        .is_stream = audio_sampler.bank_is_stream(i),
        .waveform = audio_sampler.bank_waveform(i, 256U),
        .root_note = audio_sampler.bank_root_note(i),
        .slice_count = audio_sampler.bank_slice_count(i),
        .transcribe_cached = transcribe_cached[i],
    });
  }

  return WebRuntimeState{
      .controller =
          WebControllerState{
              .left_x = controller.left_x(),
              .left_y = controller.left_y(),
              .right_x = controller.right_x(),
              .right_y = controller.right_y(),
              .left_trigger = controller.left_trigger(),
              .right_trigger = controller.right_trigger(),
              .has_gyro = controller.has_gyro(),
              .has_accel = controller.has_accel(),
              .gyro = WebVec3{.x = controller.gyro().x,
                              .y = controller.gyro().y,
                              .z = controller.gyro().z},
              .accel = WebVec3{.x = controller.accel().x,
                                .y = controller.accel().y,
                                .z = controller.accel().z},
          },
      .music =
          [&] {
            const auto sc = scale_for(intent.scale);
            std::vector<int> btn_notes(8);
            for (int i = 0; i < 8; ++i) {
              const int wrapped = i % sc.size;
              const int extra = i / sc.size;
              btn_notes[static_cast<std::size_t>(i)] = std::clamp(
                  intent.root_midi_note +
                      sc.semitones[static_cast<std::size_t>(wrapped)] +
                      (intent.octave_offset + extra) * 12,
                  0, 127);
            }
            return WebMusicState{
                .root_midi_note = intent.root_midi_note,
                .octave_offset = intent.octave_offset,
                .scale = std::string{scale_name(intent.scale)},
                .pitch_bend = intent.controls.pitch_bend,
                .expression = intent.controls.expression,
                .filter_cutoff = intent.controls.filter_cutoff,
                .filter_resonance = intent.controls.filter_resonance,
                .modulation = intent.controls.modulation,
                .vibrato = intent.controls.vibrato,
                .active_notes = active_notes,
                .midi_program = static_cast<int>(midi_program),
                .midi_bank = static_cast<int>(midi_bank),
                .button_midi_notes = std::move(btn_notes),
            };
          }(),
      .audio =
          WebAudioState{
              .devices = std::move(capture_devices),
              .selected_device_index = audio_capture.selected_device_index(),
              .capture_running = audio_capture.is_running(),
              .sample_recording = audio_capture.is_sample_recording(),
              .capture_device = audio_capture.selected_device_name(),
              .mic_level = audio_capture.level(),
              .envelope = audio_features.envelope,
              .gate_open = audio_features.gate_open,
              .onset = audio_features.onset,
              .velocity = audio_features.velocity,
              .waveform = audio_capture.waveform(),
              .sample_ready = audio_sampler.has_sample(),
              .sample_frames =
                  static_cast<std::uint32_t>(audio_sampler.sample_frames()),
              .sample_trim_start = audio_sampler.trim_start(),
              .sample_trim_end = audio_sampler.trim_end(),
              .sample_waveform = audio_sampler.sample_waveform(),
              .wavetable_cycle = audio_sampler.bank_wavetable_cycle(audio_sampler.active_bank(), 128U),
              .banks = std::move(sample_banks),
              .active_bank = audio_sampler.active_bank(),
              .touchpad_sketch = sketch.active
                                      ? build_wavetable(sketch.points, 128)
                                      : sketch.committed,
              .touchpad_drawing = sketch.active,
              .touchpad_raw_points =
                  [&] {
                    const auto &src =
                        sketch.active ? sketch.points : sketch.committed_points;
                    std::vector<std::array<float, 2>> out;
                    out.reserve(src.size());
                    for (auto [x, y] : src) {
                      out.push_back({x, y});
                    }
                    return out;
                  }(),
              .signals_volume = signals_volume,
              .blow_mode = blow_mode,
              .wavetable_morph = wavetable_morph,
              .wavetable_noise = wavetable_noise,
              .wavetable_unison = wavetable_unison,
              .blow_sensitivity = blow_sensitivity,
              .blow_active = blow_active,
              .blow_level = blow_level,
              .voice_seq_available = voice_seq_status.available,
              .voice_seq_compiled = voice_seq_status.compiled,
              .voice_seq_enabled = voice_seq_status.enabled,
              .voice_seq_recording = voice_seq_status.recording,
              .voice_seq_mode =
                  std::string{voice_mode_name(voice_seq_status.mode)},
              .voice_seq_snap = voice_seq_status.snap_to_scale,
              .voice_seq_sensitivity = voice_seq_status.sensitivity,
              .voice_seq_timing_offset_ms =
                  voice_seq_status.timing_offset_ms,
              .voice_seq_last_note = voice_seq_status.last_midi_note,
              .voice_seq_last_velocity = voice_seq_status.last_velocity,
              .voice_seq_accepted_notes = voice_seq_status.accepted_notes,
              .voice_seq_rejected_notes = voice_seq_status.rejected_notes,
              .voice_seq_recorded_segments = voice_seq_status.recorded_segments,
              .voice_seq_record_progress = voice_seq_status.record_progress,
              .spec_samples = spec_samples,
              .mic_has_sample = mic_has_sample,
              .transcribe_state = transcribe_state,
              .stem_split_state = stem_split_state,
              .stem_split_error = stem_split_error,
              .stem_split_progress = stem_split_progress,
              .stem_split_detail = stem_split_detail,
              .stem_split_log = stem_split_log,
              .stems = [&] {
                std::vector<WebStemSlot> slots{};
                const auto n = audio_sampler.loaded_stem_count();
                slots.reserve(n);
                for (std::size_t i = 0; i < n; ++i) {
                  slots.push_back(WebStemSlot{
                      .name = audio_sampler.stem_name(i),
                      .frames = audio_sampler.stem_frame_count(i),
                      .is_active = audio_sampler.stem_is_active(i),
                      .waveform = audio_sampler.stem_waveform(i, 256U),
                  });
                }
                return slots;
              }(),
              .stems_folder = stems_folder,
          },
      .seq = seq_web_state(seq),
      .piano_roll_visible = piano_roll_visible,
      .spectrogram_visible = spectrogram_visible,
  };
}

WebLibraryState make_web_library_state(
    const std::vector<WebPreset> &presets,
    const std::vector<std::string> &soundfonts,
    const std::string &active_soundfont) {
  return WebLibraryState{
      .presets = presets,
      .soundfonts = soundfonts,
      .active_soundfont = active_soundfont,
  };
}

} // namespace analogno
