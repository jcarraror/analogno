#include "sequencer.hpp"

#include <algorithm>
#include <cstddef>
#include <random>

namespace analogno {

int effective_midi_channel(const SeqTrack &track) {
  return track.midi_bank == 128 ? 9 : track.midi_channel;
}

int sequencer_step_count(int value) {
  if (value <= Sequencer::min_step_count) {
    return Sequencer::min_step_count;
  }
  if (value <= 16) {
    return 16;
  }
  if (value <= 32) {
    return 32;
  }
  return Sequencer::max_step_count;
}

int sequencer_step_division(int value) {
  if (value <= 8) {
    return 8;
  }
  if (value <= 16) {
    return 16;
  }
  return 32;
}

int track_loop_length(const SeqTrack &track, int step_count) {
  return std::clamp(track.loop_length, 1, step_count);
}

void resize_sequencer_steps(Sequencer &seq, int step_count) {
  const auto old_step_count = seq.step_count;
  seq.step_count = sequencer_step_count(step_count);
  for (auto &track : seq.tracks) {
    track.steps.resize(static_cast<std::size_t>(seq.step_count));
    if (track.loop_length == old_step_count) {
      track.loop_length = seq.step_count;
    }
    track.loop_length = std::clamp(track.loop_length, 1, seq.step_count);
  }
  if (seq.selected_step >= seq.step_count) {
    seq.selected_step = -1;
  }
  if (seq.current_step >= seq.step_count) {
    seq.current_step = -1;
    seq.playhead_step = -1;
    seq.step_start = std::chrono::steady_clock::now();
  }
}

int active_track_loop_length(const Sequencer &seq) {
  if (seq.active_track < 0 ||
      static_cast<std::size_t>(seq.active_track) >= seq.tracks.size()) {
    return seq.step_count;
  }
  return track_loop_length(seq.tracks[static_cast<std::size_t>(seq.active_track)],
                           seq.step_count);
}

std::optional<Note> note_for_step(const SeqStep &step, const SeqTrack &track,
                                  const MusicalIntent &ctx) {
  if (!step.active) {
    return std::nullopt;
  }

  const auto scale = scale_for(ctx.scale);
  const auto wrapped = step.degree % scale.size;
  const auto xoct = step.degree / scale.size;
  const auto semitone = scale.semitones[static_cast<std::size_t>(wrapped)];
  const auto computed = std::clamp(
      ctx.root_midi_note + semitone + (ctx.octave_offset + xoct) * 12, 0,
      127);
  const auto midi_note = step.midi_note >= 0 ? step.midi_note : computed;

  const auto scaled_vel = std::clamp(step.velocity * track.velocity_scale / 100, 1, 127);
  return Note{
      .midi_note = midi_note,
      .degree = step.degree,
      .octave = ctx.octave_offset + xoct,
      .velocity = scaled_vel,
      .channel = effective_midi_channel(track),
  };
}

SeqTick tick_sequencer(Sequencer &seq, const MusicalIntent &ctx) {
  if (!seq.playing) {
    return {};
  }

  const auto now = std::chrono::steady_clock::now();
  using ms = std::chrono::duration<double, std::milli>;
  const auto step_dur =
      ms{60000.0 * 4.0 / static_cast<double>(seq.bpm) /
         static_cast<double>(seq.step_division)};
  const auto gate_dur = step_dur * (seq.gate_pct / 100.0);
  const auto elapsed = std::chrono::duration_cast<ms>(now - seq.step_start);
  // Swing: even playhead steps precede an odd (swung-late) step → extended threshold.
  // Odd playhead steps precede an even (on-time) step → shortened threshold to catch up.
  const auto swing_frac = static_cast<double>(seq.swing) / 100.0;
  const auto step_threshold = (seq.playhead_step >= 0 && seq.playhead_step % 2 == 0)
      ? ms{step_dur.count() * (1.0 + swing_frac)}
      : ms{step_dur.count() * (1.0 - swing_frac)};

  SeqTick result{};

  const bool any_solo = std::any_of(seq.tracks.begin(), seq.tracks.end(),
      [](const SeqTrack &t) { return t.solo; });

  for (auto &track : seq.tracks) {
    if (track.pending_note_off.has_value() && elapsed >= gate_dur) {
      const auto loop_length = track_loop_length(track, seq.step_count);
      const auto next_step_index =
          seq.playhead_step >= 0 ? (seq.playhead_step + 1) % loop_length : 0;
      const auto &next_step =
          track.steps[static_cast<std::size_t>(next_step_index)];
      const auto next_note = note_for_step(next_step, track, ctx);
      const auto held_by_tie =
          next_step.active && next_step.tie && next_note.has_value() &&
          next_note->midi_note == track.pending_note_off->midi_note;
      if (held_by_tie) {
        continue;
      }
      if (track.sample_bank >= 0) {
        result.sample_note_offs.push_back(
            {.bank = track.sample_bank, .note = *track.pending_note_off});
      } else {
        result.note_offs.push_back(*track.pending_note_off);
      }
      track.pending_note_off.reset();
    }
  }

  if (elapsed >= step_threshold) {
    seq.playhead_step = seq.playhead_step >= 0 ? seq.playhead_step + 1 : 0;
    seq.current_step = seq.playhead_step % seq.step_count;
    const auto next_start =
        seq.step_start +
        std::chrono::round<std::chrono::steady_clock::duration>(step_threshold);
    seq.step_start = (now - next_start >
                      std::chrono::round<std::chrono::steady_clock::duration>(
                          step_threshold))
                         ? now
                         : next_start;
    result.stepped = true;

    for (auto &track : seq.tracks) {
      const auto loop_length = track_loop_length(track, seq.step_count);
      const auto track_step = seq.playhead_step % loop_length;
      const auto &step = track.steps[static_cast<std::size_t>(track_step)];
      const auto step_note = note_for_step(step, track, ctx);
      const bool effective_muted = track.muted || (any_solo && !track.solo);
      const auto tie_continues =
          !effective_muted && step.active && step.tie && step_note.has_value() &&
          track.pending_note_off.has_value() &&
          step_note->midi_note == track.pending_note_off->midi_note;

      if (track.pending_note_off.has_value() && !tie_continues) {
        if (track.sample_bank >= 0) {
          result.sample_note_offs.push_back(
              {.bank = track.sample_bank, .note = *track.pending_note_off});
        } else {
          result.note_offs.push_back(*track.pending_note_off);
        }
        track.pending_note_off.reset();
      }

      if (!tie_continues && step.active && !step.tie && !effective_muted &&
          step_note.has_value()) {
        const bool fires = step.probability >= 100 ||
            std::uniform_int_distribution<int>{1, 100}(seq.rng) <= step.probability;
        if (fires) {
          if (track.sample_bank >= 0) {
            result.sample_note_ons.push_back({
                .bank        = track.sample_bank,
                .note        = *step_note,
                .volume_gain = static_cast<float>(track.volume) / 127.0F,
                .pan         = (static_cast<float>(track.pan) - 64.0F) / 64.0F,
            });
          } else {
            result.note_ons.push_back(*step_note);
          }
          track.pending_note_off = *step_note;
        }
      }
    }
  }

  return result;
}

WebSeqState seq_web_state(const Sequencer &seq) {
  WebSeqState s{};
  s.playing = seq.playing;
  s.active_track = seq.active_track;
  s.selected_step = seq.selected_step;
  s.bpm = seq.bpm;
  s.playhead_step = seq.playhead_step;
  s.current_step = seq.current_step;
  s.gate_pct = seq.gate_pct;
  s.step_count = seq.step_count;
  s.step_division = seq.step_division;
  s.swing = seq.swing;
  s.clipboard_available = seq.clipboard.has_value();
  for (const auto &track : seq.tracks) {
    WebSeqTrack wt{};
    wt.midi_channel = track.midi_channel;
    wt.midi_program = track.midi_program;
    wt.midi_bank = track.midi_bank;
    wt.sample_bank = track.sample_bank;
    wt.loop_length = track_loop_length(track, seq.step_count);
    wt.volume = track.volume;
    wt.pan = track.pan;
    wt.velocity_scale = track.velocity_scale;
    wt.muted = track.muted;
    wt.solo = track.solo;
    wt.name = track.name;
    wt.steps.reserve(static_cast<std::size_t>(seq.step_count));
    for (const auto &step : track.steps) {
      wt.steps.push_back({step.active, step.tie, step.degree, step.velocity,
                          step.midi_note, step.probability});
    }
    s.tracks.push_back(std::move(wt));
  }
  return s;
}

void apply_voice_pattern_to_seq(Sequencer &seq,
                                const VoiceSequencerPattern &pattern) {
  if (seq.active_track < 0 ||
      static_cast<std::size_t>(seq.active_track) >= seq.tracks.size()) {
    return;
  }

  auto &track = seq.tracks[static_cast<std::size_t>(seq.active_track)];
  for (std::size_t i = 0;
       i < track.steps.size() && i < pattern.steps.size(); ++i) {
    const auto &src = pattern.steps[i];
    if (!src.active) {
      track.steps[i] = {};
      continue;
    }

    track.steps[i] = SeqStep{
        .active = true,
        .tie = src.tie,
        .degree = src.note.degree,
        .velocity = src.note.velocity,
        .midi_note = src.note.midi_note,
    };
  }
}

} // namespace analogno
