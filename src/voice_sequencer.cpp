#include "voice_sequencer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#if ANALOGNO_HAVE_AUBIO
#include <aubio/aubio.h>
#endif

namespace analogno {
namespace {

constexpr auto sample_rate = unsigned{48000};
constexpr auto buffer_size = unsigned{2048};
constexpr auto hop_size = unsigned{512};
constexpr auto max_drain_frames = std::size_t{8192};
constexpr auto min_segment_frames = std::uint64_t{2400};
constexpr auto event_fuse_frames = std::uint64_t{3600};
constexpr auto default_input_latency_frames = std::int64_t{2048};

#if ANALOGNO_HAVE_AUBIO
struct ScaleMatch final {
  int midi_note{};
  int degree{};
  int octave{};
};

ScaleMatch nearest_scale_match(int midi_note, int root_midi_note,
                               int octave_offset, ScaleKind kind) {
  const auto scale = scale_for(kind);
  auto best = ScaleMatch{
      .midi_note = std::clamp(midi_note, 0, 127),
      .degree = 0,
      .octave = octave_offset,
  };
  auto best_distance = std::numeric_limits<int>::max();

  for (int degree = -scale.size * 8; degree <= scale.size * 8; ++degree) {
    const auto wrapped =
        ((degree % scale.size) + scale.size) % scale.size;
    const auto degree_octave =
        degree >= 0 ? degree / scale.size
                    : -(((-degree) + scale.size - 1) / scale.size);
    const auto candidate = std::clamp(
        root_midi_note +
            scale.semitones[static_cast<std::size_t>(wrapped)] +
            (octave_offset + degree_octave) * 12,
        0, 127);
    const auto distance = std::abs(candidate - midi_note);

    if (distance < best_distance ||
        (distance == best_distance && candidate < best.midi_note)) {
      best = ScaleMatch{
          .midi_note = candidate,
          .degree = degree,
          .octave = octave_offset + degree_octave,
      };
      best_distance = distance;
    }
  }

  return best;
}

float silence_db_for_sensitivity(float sensitivity) {
  const auto clamped = std::clamp(sensitivity, 0.0F, 1.0F);
  return -30.0F - clamped * 55.0F;
}

float onset_threshold_for_sensitivity(float sensitivity) {
  const auto clamped = std::clamp(sensitivity, 0.0F, 1.0F);
  return 0.58F - clamped * 0.38F;
}
#endif

} // namespace

void VoiceSequencer::add_event(const Note &note, std::uint64_t frame,
                               bool pitched) {
  for (auto &event : events_) {
    const auto lo = std::min(event.frame, frame);
    const auto hi = std::max(event.frame, frame);
    if (hi - lo > event_fuse_frames) {
      continue;
    }

    if (pitched && !event.pitched) {
      event.note = note;
      event.frame = std::min(event.frame, frame);
      event.pitched = true;
    } else if (note.velocity > event.note.velocity) {
      event.note.velocity = note.velocity;
      event.frame = std::min(event.frame, frame);
    }
    return;
  }

  events_.push_back(TimedEvent{
      .note = note,
      .frame = frame,
      .pitched = pitched,
  });
}

struct VoiceSequencer::Impl final {
#if ANALOGNO_HAVE_AUBIO
  aubio_notes_t *notes{};
  aubio_onset_t *onset{};
  fvec_t *input{};
  fvec_t *output{};
  fvec_t *onset_output{};
#endif
  std::vector<float> pending{};
  bool available{};

  Impl() {
#if ANALOGNO_HAVE_AUBIO
    notes = new_aubio_notes("default", buffer_size, hop_size, sample_rate);
    onset = new_aubio_onset("hfc", buffer_size, hop_size, sample_rate);
    input = new_fvec(hop_size);
    output = new_fvec(3);
    onset_output = new_fvec(1);
    available = notes != nullptr && onset != nullptr && input != nullptr &&
                output != nullptr && onset_output != nullptr;

    if (available) {
      aubio_notes_set_minioi_ms(notes, 35.0F);
      aubio_notes_set_release_drop(notes, 12.0F);
      aubio_notes_set_silence(notes, silence_db_for_sensitivity(0.65F));
      aubio_onset_set_minioi_ms(onset, 22.0F);
      aubio_onset_set_threshold(onset, onset_threshold_for_sensitivity(0.65F));
      aubio_onset_set_silence(onset, silence_db_for_sensitivity(0.65F));
      aubio_onset_set_awhitening(onset, 1);
    }
#else
    available = false;
#endif
    pending.reserve(hop_size * 2);
  }

  ~Impl() {
#if ANALOGNO_HAVE_AUBIO
    if (onset_output != nullptr) {
      del_fvec(onset_output);
    }
    if (output != nullptr) {
      del_fvec(output);
    }
    if (input != nullptr) {
      del_fvec(input);
    }
    if (notes != nullptr) {
      del_aubio_notes(notes);
    }
    if (onset != nullptr) {
      del_aubio_onset(onset);
    }
#endif
  }

  void set_sensitivity(float sensitivity) {
#if ANALOGNO_HAVE_AUBIO
    if (notes != nullptr) {
      aubio_notes_set_silence(notes, silence_db_for_sensitivity(sensitivity));
    }
    if (onset != nullptr) {
      aubio_onset_set_silence(onset, silence_db_for_sensitivity(sensitivity));
      aubio_onset_set_threshold(onset,
                                onset_threshold_for_sensitivity(sensitivity));
    }
#else
    static_cast<void>(sensitivity);
#endif
  }
};

VoiceSequencer::VoiceSequencer() : impl_{new Impl{}} {}

VoiceSequencer::~VoiceSequencer() { delete impl_; }

void VoiceSequencer::configure(const VoiceSequencerConfig &config) {
  config_ = VoiceSequencerConfig{
      .enabled = config.enabled,
      .mode = config.mode,
      .snap_to_scale = config.snap_to_scale,
      .sensitivity = std::clamp(config.sensitivity, 0.0F, 1.0F),
      .timing_offset_ms =
          std::clamp(config.timing_offset_ms, -120.0F, 120.0F),
  };

  impl_->set_sensitivity(config_.sensitivity);

  if (!config_.enabled) {
    impl_->pending.clear();
    last_midi_note_ = -1;
    last_velocity_ = 0;
  }
}

const VoiceSequencerConfig &VoiceSequencer::config() const { return config_; }

VoiceSequencerStatus VoiceSequencer::status() const {
  const auto record_frames =
      static_cast<float>(sample_rate) * 60.0F /
      std::max(record_bpm_, 1.0F) / 4.0F *
      static_cast<float>(record_step_count_);
  const auto progress =
      recording_ && record_frames > 0.0F
          ? std::clamp(static_cast<float>(frame_cursor_ - record_start_frame_) /
                           record_frames,
                       0.0F, 1.0F)
          : 0.0F;

  return VoiceSequencerStatus{
      .compiled =
#if ANALOGNO_HAVE_AUBIO
          true,
#else
          false,
#endif
      .available = impl_->available,
      .enabled = config_.enabled,
      .recording = recording_,
      .mode = config_.mode,
      .snap_to_scale = config_.snap_to_scale,
      .sensitivity = config_.sensitivity,
      .timing_offset_ms = config_.timing_offset_ms,
      .last_midi_note = last_midi_note_,
      .last_velocity = last_velocity_,
      .accepted_notes = accepted_notes_,
      .rejected_notes = rejected_notes_,
      .recorded_segments = events_.size(),
      .record_progress = progress,
  };
}

void VoiceSequencer::start_recording(float bpm, int step_count) {
  record_bpm_ = std::clamp(bpm, 20.0F, 300.0F);
  record_step_count_ = std::clamp(step_count, 1, 64);
  record_start_frame_ = frame_cursor_;
  segments_.clear();
  events_.clear();
  active_segment_.reset();
  recording_ = true;
}

std::optional<VoiceSequencerPattern> VoiceSequencer::stop_recording(
    int root_midi_note, int octave_offset, ScaleKind scale) {
  if (!recording_) {
    return std::nullopt;
  }

  if (active_segment_.has_value()) {
    active_segment_->end_frame = frame_cursor_;
    if (active_segment_->end_frame > active_segment_->start_frame &&
        active_segment_->end_frame - active_segment_->start_frame >=
            min_segment_frames) {
      segments_.push_back(*active_segment_);
    }
    active_segment_.reset();
  }

  recording_ = false;

  VoiceSequencerPattern pattern{};
  pattern.steps.resize(static_cast<std::size_t>(record_step_count_));
  pattern.segment_count = segments_.size() + events_.size();

  const auto frames_per_step =
      static_cast<double>(sample_rate) * 60.0 /
      static_cast<double>(record_bpm_) / 4.0;
  const auto record_end_frame =
      record_start_frame_ + static_cast<std::uint64_t>(
                                std::llround(frames_per_step *
                                             static_cast<double>(
                                                 record_step_count_)));

  auto quantize_step = [&](std::uint64_t frame) {
    const auto offset_frames =
        static_cast<std::int64_t>(std::llround(
            static_cast<double>(config_.timing_offset_ms) *
            static_cast<double>(sample_rate) / 1000.0));
    const auto correction = default_input_latency_frames - offset_frames;
    const auto adjusted =
        static_cast<std::int64_t>(frame) > correction
            ? static_cast<std::uint64_t>(
                  static_cast<std::int64_t>(frame) - correction)
            : std::uint64_t{0};
    const auto clamped = std::clamp(adjusted, record_start_frame_,
                                    record_end_frame);
    auto step = static_cast<int>(std::floor(
        (static_cast<double>(clamped - record_start_frame_) +
         frames_per_step * 0.5) /
        frames_per_step));
    return std::clamp(step, 0, record_step_count_ - 1);
  };

  if (config_.mode != VoiceSequencerMode::percussion) {
  for (const auto &segment : segments_) {
    const auto start = std::max(segment.start_frame, record_start_frame_);
    const auto end = std::min(segment.end_frame, record_end_frame);
    if (end <= start) {
      continue;
    }

    auto step_start = quantize_step(start);
    auto step_end = static_cast<int>(std::ceil(
        static_cast<double>(end - record_start_frame_) / frames_per_step));

    step_start = std::clamp(step_start, 0, record_step_count_ - 1);
    step_end = std::clamp(step_end, step_start + 1, record_step_count_);

    auto note = segment.note;
    if (config_.snap_to_scale) {
#if ANALOGNO_HAVE_AUBIO
      const auto match =
          nearest_scale_match(note.midi_note, root_midi_note, octave_offset,
                              scale);
      note.midi_note = match.midi_note;
      note.degree = std::max(0, match.degree);
      note.octave = match.octave;
#else
      static_cast<void>(root_midi_note);
      static_cast<void>(octave_offset);
      static_cast<void>(scale);
#endif
    }

    auto &start_step = pattern.steps[static_cast<std::size_t>(step_start)];
    const auto should_replace =
        !start_step.active || note.velocity >= start_step.note.velocity;
    if (!should_replace) {
      continue;
    }

    start_step = VoiceSequencerStep{
        .active = true,
        .tie = false,
        .note = note,
    };

    for (int i = step_start + 1; i < step_end; ++i) {
      pattern.steps[static_cast<std::size_t>(i)] = VoiceSequencerStep{
          .active = true,
          .tie = true,
          .note = note,
      };
    }
  }
  }

  for (const auto &event : events_) {
    if (config_.mode == VoiceSequencerMode::percussion && event.pitched) {
      continue;
    }
    if (config_.mode == VoiceSequencerMode::harmonic && !event.pitched) {
      continue;
    }

    const auto step = quantize_step(event.frame);

    auto &dst = pattern.steps[static_cast<std::size_t>(step)];
    if (dst.active && !dst.tie && dst.note.velocity > event.note.velocity) {
      continue;
    }

    if (dst.tie) {
      continue;
    }

    dst = VoiceSequencerStep{
        .active = true,
        .tie = false,
        .note = event.note,
    };
  }

  segments_.clear();
  events_.clear();
  return pattern;
}

void VoiceSequencer::process(AudioCapture &capture, int root_midi_note,
                             int octave_offset, ScaleKind scale) {
  static_cast<void>(root_midi_note);
  static_cast<void>(octave_offset);
  static_cast<void>(scale);

  if (!config_.enabled || !impl_->available) {
    auto drained = capture.consume_analysis_frames(max_drain_frames);
    static_cast<void>(drained);
    return;
  }

  auto frames = capture.consume_analysis_frames(max_drain_frames);
  if (frames.empty()) {
    return;
  }

  impl_->pending.insert(impl_->pending.end(), frames.begin(), frames.end());

  while (impl_->pending.size() >= hop_size) {
#if ANALOGNO_HAVE_AUBIO
    std::copy_n(impl_->pending.begin(), hop_size, impl_->input->data);

    auto sum = 0.0F;
    for (std::size_t i = 0; i < hop_size; ++i) {
      sum += impl_->input->data[i] * impl_->input->data[i];
    }
    const auto rms =
        std::sqrt(sum / static_cast<float>(hop_size));
    const auto hit_velocity =
        std::clamp(25 + static_cast<int>(std::lround(rms * 950.0F)), 1, 127);

    aubio_notes_do(impl_->notes, impl_->input, impl_->output);
    aubio_onset_do(impl_->onset, impl_->input, impl_->onset_output);

    frame_cursor_ += hop_size;
    const auto event_frame = frame_cursor_;
    const auto onset_detected = impl_->onset_output->data[0] > 0.0F;
    const auto midi = static_cast<int>(std::lround(impl_->output->data[0]));
    const auto velocity =
        static_cast<int>(std::lround(impl_->output->data[1]));
    const auto off_midi =
        static_cast<int>(std::lround(impl_->output->data[2]));

    if (off_midi > 0 && active_segment_.has_value()) {
      active_segment_->end_frame = event_frame;
      if (active_segment_->end_frame > active_segment_->start_frame &&
          active_segment_->end_frame - active_segment_->start_frame >=
              min_segment_frames) {
        segments_.push_back(*active_segment_);
        add_event(active_segment_->note, active_segment_->start_frame, true);
      }
      active_segment_.reset();
    }

    if (recording_ && onset_detected &&
        config_.mode != VoiceSequencerMode::harmonic) {
      const auto onset_frame = static_cast<std::uint64_t>(
          std::min<unsigned>(aubio_onset_get_last(impl_->onset),
                             static_cast<unsigned>(frame_cursor_)));
      auto note = Note{
          .midi_note = last_midi_note_ >= 0 ? last_midi_note_ : root_midi_note,
          .degree = 0,
          .octave = octave_offset,
          .velocity = hit_velocity,
          .channel = 0,
      };

      if (config_.snap_to_scale) {
        const auto match =
            nearest_scale_match(note.midi_note, root_midi_note, octave_offset,
                                scale);
        note.midi_note = match.midi_note;
        note.degree = std::max(0, match.degree);
        note.octave = match.octave;
      }

      if (onset_frame + event_fuse_frames >= record_start_frame_) {
        add_event(note, onset_frame, false);
      }
    }

    if (midi > 0 && velocity > 0 &&
        config_.mode != VoiceSequencerMode::percussion) {
      if (midi < 12 || midi > 108) {
        ++rejected_notes_;
      } else {
        if (active_segment_.has_value()) {
          active_segment_->end_frame = event_frame;
          if (active_segment_->end_frame > active_segment_->start_frame &&
              active_segment_->end_frame - active_segment_->start_frame >=
                  min_segment_frames) {
            segments_.push_back(*active_segment_);
            add_event(active_segment_->note, active_segment_->start_frame,
                      true);
          }
        }

        auto note = Note{
            .midi_note = std::clamp(midi, 0, 127),
            .degree = 0,
            .octave = octave_offset,
            .velocity = std::clamp(velocity, 1, 127),
            .channel = 0,
        };

        if (config_.snap_to_scale) {
          const auto match =
              nearest_scale_match(note.midi_note, root_midi_note,
                                  octave_offset, scale);
          note.midi_note = match.midi_note;
          note.degree = std::max(0, match.degree);
          note.octave = match.octave;
        }

        if (recording_) {
          add_event(note, event_frame, true);
          active_segment_ = Segment{
              .note = note,
              .start_frame = event_frame,
              .end_frame = event_frame,
          };
        }

        last_midi_note_ = note.midi_note;
        last_velocity_ = note.velocity;
        ++accepted_notes_;
      }
    }
#endif

    impl_->pending.erase(impl_->pending.begin(),
                         impl_->pending.begin() +
                             static_cast<std::ptrdiff_t>(hop_size));
  }
}

} // namespace analogno
