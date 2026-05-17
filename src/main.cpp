#include "audio_capture.hpp"
#include "audio_sampler.hpp"
#include "controller_state.hpp"
#include "midi_output.hpp"
#include "music_mapper.hpp"
#include "music_types.hpp"
#include "sdl_check.hpp"
#include "sdl_gamepad.hpp"
#include "sf2_reader.hpp"
#include "voice_sequencer.hpp"
#include "web_server.hpp"
#include "web_state.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <deque>
#include <vector>

namespace {

using analogno::AudioCapture;
using analogno::AudioSampler;
using analogno::ContinuousControls;
using analogno::ControllerState;
using analogno::Gamepad;
using analogno::MidiOutput;
using analogno::MusicalIntent;
using analogno::MusicMapper;
using analogno::Note;
using analogno::WebSocketServer;


constexpr auto wavetable_length = std::size_t{367};

analogno::VoiceSequencerMode voice_mode_from_string(std::string_view mode) {
  if (mode == "harmonic") {
    return analogno::VoiceSequencerMode::harmonic;
  }
  if (mode == "hybrid") {
    return analogno::VoiceSequencerMode::hybrid;
  }
  return analogno::VoiceSequencerMode::percussion;
}

std::string_view voice_mode_name(analogno::VoiceSequencerMode mode) {
  switch (mode) {
  case analogno::VoiceSequencerMode::harmonic:
    return "harmonic";
  case analogno::VoiceSequencerMode::hybrid:
    return "hybrid";
  case analogno::VoiceSequencerMode::percussion:
    return "percussion";
  }

  return "percussion";
}

struct WaveformSketch {
  bool active{false};
  std::vector<std::pair<float, float>> points{};
  std::vector<float> committed{};
  std::vector<std::pair<float, float>> committed_points{};
};


std::vector<float> build_wavetable(
    const std::vector<std::pair<float, float>> &points, std::size_t n) {
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

  // Find first filled bin.
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
      [](float a, float b) {
        return std::abs(a) < std::abs(b);
      }));

  if (max_abs < 0.01F) {
    return {};
  }

  for (float &v : result) {
    v /= max_abs;
  }

  return result;
}

struct Sdl final {
  Sdl() {
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
      analogno::fail_sdl("SDL_Init failed");
    }
  }

  ~Sdl() { SDL_Quit(); }

  Sdl(const Sdl &) = delete;
  Sdl &operator=(const Sdl &) = delete;
  Sdl(Sdl &&) = delete;
  Sdl &operator=(Sdl &&) = delete;
};

class ActiveNoteTracker final {
public:
  void apply(const MusicalIntent &intent) {
    if (intent.note_off_all) {
      active_.fill(false);
    }

    for (const auto &note : intent.note_offs) {
      if (valid(note.midi_note)) {
        active_[static_cast<std::size_t>(note.midi_note)] = false;
      }
    }

    for (const auto &note : intent.note_ons) {
      if (valid(note.midi_note)) {
        active_[static_cast<std::size_t>(note.midi_note)] = true;
      }
    }
  }

  [[nodiscard]] std::vector<int> notes() const {
    std::vector<int> result{};

    for (std::size_t i = 0; i < active_.size(); ++i) {
      if (active_[i]) {
        result.push_back(static_cast<int>(i));
      }
    }

    return result;
  }

private:
  std::array<bool, 128> active_{};

  static bool valid(int note) { return note >= 0 && note < 128; }
};

std::optional<Gamepad> open_first_gamepad() {
  int count = 0;
  SDL_JoystickID *ids = SDL_GetGamepads(&count);

  if (ids == nullptr) {
    analogno::fail_sdl("SDL_GetGamepads failed");
  }

  std::unique_ptr<SDL_JoystickID[], decltype(&SDL_free)> ids_owner{
      ids,
      SDL_free,
  };

  if (count == 0) {
    std::cout << "no gamepads found\n";
    return std::nullopt;
  }

  std::cout << "gamepads found: " << count << '\n';

  const std::span<const SDL_JoystickID> gamepad_ids{
      ids,
      static_cast<std::size_t>(count),
  };

  for (const SDL_JoystickID id : gamepad_ids) {
    const char *name = SDL_GetGamepadNameForID(id);
    std::cout << "  id=" << id
              << " name=" << (name != nullptr ? name : "unknown") << '\n';
  }

  return Gamepad{gamepad_ids.front()};
}

void print_capabilities(const Gamepad &gamepad) {
  std::cout << '\n';
  std::cout << "opened gamepad\n";
  std::cout << "  id: " << gamepad.id() << '\n';
  std::cout << "  name: " << gamepad.name() << '\n';
  std::cout << "  type: " << analogno::gamepad_type_name(gamepad.type())
            << '\n';
  std::cout << "  touchpads: " << gamepad.touchpad_count() << '\n';
  std::cout << "  has gyro: " << std::boolalpha
            << gamepad.has_sensor(SDL_SENSOR_GYRO) << '\n';
  std::cout << "  has accelerometer: " << std::boolalpha
            << gamepad.has_sensor(SDL_SENSOR_ACCEL) << '\n';
  std::cout << '\n';
}

void enable_motion_sensors(const Gamepad &gamepad) {
  gamepad.enable_sensor(SDL_SENSOR_GYRO, "gyro");
  gamepad.enable_sensor(SDL_SENSOR_ACCEL, "accelerometer");
  std::cout << '\n';
}

void print_note_on(const analogno::Note &note) {
  std::cout << "note_on"
            << " midi=" << note.midi_note << " degree=" << note.degree
            << " octave=" << note.octave << '\n';
}

void print_note_off(const analogno::Note &note) {
  std::cout << "note_off"
            << " midi=" << note.midi_note << " degree=" << note.degree
            << " octave=" << note.octave << '\n';
}

bool has_note_event(const MusicalIntent &intent) {
  return !intent.note_ons.empty() || !intent.note_offs.empty() ||
         intent.note_off_all;
}

void print_intent(const MusicalIntent &intent) {
  std::cout << std::fixed << std::setprecision(3) << "intent"
            << " root=" << intent.root_midi_note
            << " octave_offset=" << intent.octave_offset
            << " scale=" << analogno::scale_name(intent.scale)
            << " pitch_bend=" << intent.controls.pitch_bend
            << " expression=" << intent.controls.expression
            << " cutoff=" << intent.controls.filter_cutoff
            << " resonance=" << intent.controls.filter_resonance
            << " modulation=" << intent.controls.modulation
            << " vibrato=" << intent.controls.vibrato << '\n';

  for (const auto &note : intent.note_ons) {
    print_note_on(note);
  }

  for (const auto &note : intent.note_offs) {
    print_note_off(note);
  }

  if (intent.note_off_all) {
    std::cout << "note_off_all\n";
  }
}

bool handle_event(const SDL_Event &event, ControllerState &state) {
  switch (event.type) {
  case SDL_EVENT_QUIT:
    return false;

  case SDL_EVENT_GAMEPAD_ADDED:
    std::cout << "gamepad added id=" << event.gdevice.which << '\n';
    break;

  case SDL_EVENT_GAMEPAD_REMOVED:
    std::cout << "gamepad removed id=" << event.gdevice.which << '\n';
    break;

  case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    state.handle_button_down(event.gbutton);
    break;

  case SDL_EVENT_GAMEPAD_BUTTON_UP:
    state.handle_button_up(event.gbutton);
    break;

  case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    state.handle_axis(event.gaxis);
    break;

  case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
    state.handle_sensor(event.gsensor);
    break;

  case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    state.handle_touchpad_down(event.gtouchpad);
    break;

  case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    state.handle_touchpad_motion(event.gtouchpad);
    break;

  case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    state.handle_touchpad_up(event.gtouchpad);
    break;

  default:
    break;
  }

  return true;
}

void trigger_sampler_notes(const MusicalIntent &intent, AudioSampler &sampler) {
  constexpr auto sampler_root_midi_note = 48;

  if (!sampler.has_sample()) {
    return;
  }

  for (const auto &note : intent.note_ons) {
    const auto semitones = note.midi_note - sampler_root_midi_note;
    sampler.trigger(std::pow(2.0F, static_cast<float>(semitones) / 12.0F));
  }
}

void update_sampler_controls(const ContinuousControls &controls,
                             AudioSampler &sampler,
                             bool seq_playing,
                             bool l2_controls_gain) {
  constexpr auto sampler_gain_boost = 1.25F;
  const auto live_gain = l2_controls_gain ? controls.expression : 1.0F;
  const auto gain = (seq_playing ? 1.0F : live_gain) *
                    sampler_gain_boost;
  sampler.set_gain(gain);
  sampler.set_pitch_controls(controls.pitch_bend, controls.vibrato);
}

bool sample_record_button_active(const ControllerState &controller) {
  return controller.button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) &&
         controller.button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
}

// ---------------------------------------------------------------------------
// Step sequencer
// ---------------------------------------------------------------------------

struct SeqStep {
  bool active{false};
  bool tie{false};
  int degree{0};
  int velocity{100};
  int midi_note{-1}; // -1 = compute from root/scale/octave; >=0 = absolute pitch
};

struct SeqTrack {
  int midi_channel{0}; // persists across track reordering
  int midi_program{0};
  int midi_bank{0};
  int loop_length{32};
  bool muted{false};
  std::vector<SeqStep> steps = std::vector<SeqStep>(32);
  std::optional<analogno::Note> pending_note_off{};
};

int effective_midi_channel(const SeqTrack &track) {
  return track.midi_bank == 128 ? 9 : track.midi_channel;
}

struct Sequencer {
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
    for (int i = 0; i < 4; ++i) v[static_cast<std::size_t>(i)].midi_channel = i;
    return v;
  }();
};

struct SeqTick {
  std::vector<analogno::Note> note_ons{};
  std::vector<analogno::Note> note_offs{};
  bool stepped{false}; // true the frame a new step fires
};

int sequencer_step_count(int value) {
  if (value <= Sequencer::min_step_count) return Sequencer::min_step_count;
  if (value <= 16) return 16;
  if (value <= 32) return 32;
  return Sequencer::max_step_count;
}

int sequencer_step_division(int value) {
  if (value <= 8) return 8;
  if (value <= 16) return 16;
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

std::optional<analogno::Note> note_for_step(const SeqStep &step,
                                            const SeqTrack &track,
                                            const analogno::MusicalIntent &ctx) {
  if (!step.active) {
    return std::nullopt;
  }

  const auto scale = analogno::scale_for(ctx.scale);
  const auto wrapped = step.degree % scale.size;
  const auto xoct = step.degree / scale.size;
  const auto semitone = scale.semitones[static_cast<std::size_t>(wrapped)];
  const auto computed = std::clamp(
      ctx.root_midi_note + semitone + (ctx.octave_offset + xoct) * 12, 0,
      127);
  const auto midi_note = step.midi_note >= 0 ? step.midi_note : computed;

  return analogno::Note{
      .midi_note = midi_note,
      .degree = step.degree,
      .octave = ctx.octave_offset + xoct,
      .velocity = step.velocity,
      .channel = effective_midi_channel(track),
  };
}

SeqTick tick_sequencer(Sequencer &seq, const analogno::MusicalIntent &ctx) {
  if (!seq.playing) return {};

  const auto now = std::chrono::steady_clock::now();
  using ms = std::chrono::duration<double, std::milli>;
  const auto step_dur =
      ms{60000.0 * 4.0 / static_cast<double>(seq.bpm) /
         static_cast<double>(seq.step_division)};
  const auto gate_dur = step_dur * (seq.gate_pct / 100.0);
  const auto elapsed  = std::chrono::duration_cast<ms>(now - seq.step_start);

  SeqTick result{};

  // Gate close — per track
  for (auto &track : seq.tracks) {
    if (track.pending_note_off.has_value() && elapsed >= gate_dur) {
      const auto loop_length = track_loop_length(track, seq.step_count);
      const auto next_step_index =
          seq.playhead_step >= 0
              ? (seq.playhead_step + 1) % loop_length
              : 0;
      const auto &next_step =
          track.steps[static_cast<std::size_t>(next_step_index)];
      const auto next_note = note_for_step(next_step, track, ctx);
      const auto held_by_tie =
          next_step.active && next_step.tie && next_note.has_value() &&
          next_note->midi_note == track.pending_note_off->midi_note;
      if (held_by_tie) {
        continue;
      }
      result.note_offs.push_back(*track.pending_note_off);
      track.pending_note_off.reset();
    }
  }

  if (elapsed >= step_dur) {
    seq.playhead_step = seq.playhead_step >= 0 ? seq.playhead_step + 1 : 0;
    seq.current_step = seq.playhead_step % seq.step_count;
    seq.step_start = now;
    result.stepped = true;

    for (std::size_t t = 0; t < seq.tracks.size(); ++t) {
      auto &track = seq.tracks[t];
      const auto loop_length = track_loop_length(track, seq.step_count);
      const auto track_step = seq.playhead_step % loop_length;
      const auto &step = track.steps[static_cast<std::size_t>(track_step)];
      const auto step_note = note_for_step(step, track, ctx);
      const auto tie_continues =
          !track.muted && step.active && step.tie && step_note.has_value() &&
          track.pending_note_off.has_value() &&
          step_note->midi_note == track.pending_note_off->midi_note;

      if (track.pending_note_off.has_value() && !tie_continues) {
        result.note_offs.push_back(*track.pending_note_off);
        track.pending_note_off.reset();
      }

      if (!tie_continues && step.active && !step.tie && !track.muted &&
          step_note.has_value()) {
        result.note_ons.push_back(*step_note);
        track.pending_note_off = *step_note;
      }
    }
  }

  return result;
}

analogno::WebSeqState seq_web_state(const Sequencer &seq) {
  analogno::WebSeqState s{};
  s.playing       = seq.playing;
  s.active_track  = seq.active_track;
  s.selected_step = seq.selected_step;
  s.bpm           = seq.bpm;
  s.playhead_step = seq.playhead_step;
  s.current_step  = seq.current_step;
  s.gate_pct      = seq.gate_pct;
  s.step_count    = seq.step_count;
  s.step_division = seq.step_division;
  for (const auto &track : seq.tracks) {
    analogno::WebSeqTrack wt{};
    wt.midi_channel = track.midi_channel;
    wt.midi_program = track.midi_program;
    wt.midi_bank    = track.midi_bank;
    wt.loop_length  = track_loop_length(track, seq.step_count);
    wt.muted        = track.muted;
    wt.steps.reserve(static_cast<std::size_t>(seq.step_count));
    for (const auto &step : track.steps) {
      wt.steps.push_back({step.active, step.tie, step.degree, step.velocity,
                          step.midi_note});
    }
    s.tracks.push_back(std::move(wt));
  }
  return s;
}

void apply_voice_pattern_to_seq(Sequencer &seq,
                                const analogno::VoiceSequencerPattern &pattern) {
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

// ---------------------------------------------------------------------------

analogno::WebRuntimeState
make_web_state(const ControllerState &controller, const MusicalIntent &intent,
               const ActiveNoteTracker &active_notes,
               const AudioCapture &audio_capture,
               const AudioSampler &audio_sampler,
               const analogno::AudioFeatures &audio_features,
               int midi_program,
               int midi_bank,
               const WaveformSketch &sketch,
               const Sequencer &seq,
               const std::vector<analogno::WebPreset> &sf2_presets,
               const std::vector<std::string> &available_soundfonts,
               const std::string &active_soundfont,
               bool piano_roll_visible,
               bool spectrogram_visible,
               bool blow_mode,
               float wavetable_morph,
               float wavetable_noise,
               float wavetable_unison,
               float blow_sensitivity,
               bool blow_active,
               float blow_level,
               const analogno::VoiceSequencerStatus &voice_seq_status,
               const std::vector<float>& spec_samples) {
  std::vector<analogno::WebCaptureDevice> capture_devices{};

  for (const auto &device : audio_capture.devices()) {
    capture_devices.push_back(analogno::WebCaptureDevice{
        .index = device.index,
        .name = device.name,
        .is_default = device.is_default,
    });
  }

  std::vector<analogno::WebSampleBank> sample_banks{};
  sample_banks.reserve(AudioSampler::bank_count);

  for (std::size_t i = 0; i < AudioSampler::bank_count; ++i) {
    sample_banks.push_back(analogno::WebSampleBank{
        .has_sample = audio_sampler.bank_has_sample(i),
        .frames = static_cast<std::uint32_t>(audio_sampler.bank_frames(i)),
        .trim_start = audio_sampler.bank_trim_start(i),
        .trim_end = audio_sampler.bank_trim_end(i),
        .is_wavetable = audio_sampler.bank_is_wavetable(i),
    });
  }

  return analogno::WebRuntimeState{
      .controller =
          analogno::WebControllerState{
              .left_x = controller.left_x(),
              .left_y = controller.left_y(),
              .right_x = controller.right_x(),
              .right_y = controller.right_y(),
              .left_trigger = controller.left_trigger(),
              .right_trigger = controller.right_trigger(),
              .has_gyro = controller.has_gyro(),
              .has_accel = controller.has_accel(),
              .gyro =
                  analogno::WebVec3{
                      .x = controller.gyro().x,
                      .y = controller.gyro().y,
                      .z = controller.gyro().z,
                  },
              .accel =
                  analogno::WebVec3{
                      .x = controller.accel().x,
                      .y = controller.accel().y,
                      .z = controller.accel().z,
                  },
          },
      .music = [&] {
          const auto sc = analogno::scale_for(intent.scale);
          std::vector<int> btn_notes(8);
          for (int i = 0; i < 8; ++i) {
            const int wrapped = i % sc.size;
            const int extra   = i / sc.size;
            btn_notes[static_cast<std::size_t>(i)] = std::clamp(
                intent.root_midi_note +
                sc.semitones[static_cast<std::size_t>(wrapped)] +
                (intent.octave_offset + extra) * 12,
                0, 127);
          }
          return analogno::WebMusicState{
              .root_midi_note = intent.root_midi_note,
              .octave_offset = intent.octave_offset,
              .scale = std::string{analogno::scale_name(intent.scale)},
              .pitch_bend = intent.controls.pitch_bend,
              .expression = intent.controls.expression,
              .filter_cutoff = intent.controls.filter_cutoff,
              .filter_resonance = intent.controls.filter_resonance,
              .modulation = intent.controls.modulation,
              .vibrato = intent.controls.vibrato,
              .active_notes = active_notes.notes(),
              .midi_program = static_cast<int>(midi_program),
              .midi_bank = static_cast<int>(midi_bank),
              .button_midi_notes = std::move(btn_notes),
          };
      }(),
      .audio =
          analogno::WebAudioState{
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
              .banks = std::move(sample_banks),
              .active_bank = audio_sampler.active_bank(),
              .touchpad_sketch = sketch.active
                    ? build_wavetable(sketch.points, 128)
                    : sketch.committed,
              .touchpad_drawing = sketch.active,
              .touchpad_raw_points = [&] {
                const auto &src = sketch.active ? sketch.points : sketch.committed_points;
                std::vector<std::array<float, 2>> out;
                out.reserve(src.size());
                for (auto [x, y] : src) out.push_back({x, y});
                return out;
              }(),
              .blow_mode        = blow_mode,
              .wavetable_morph  = wavetable_morph,
              .wavetable_noise  = wavetable_noise,
              .wavetable_unison = wavetable_unison,
              .blow_sensitivity  = blow_sensitivity,
              .blow_active       = blow_active,
              .blow_level        = blow_level,
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
              .spec_samples      = spec_samples,
          },
      .seq = seq_web_state(seq),
      .presets = sf2_presets,
      .soundfonts = available_soundfonts,
      .active_soundfont = active_soundfont,
      .piano_roll_visible = piano_roll_visible,
      .spectrogram_visible = spectrogram_visible,
  };
}

// Map a GM program number (0-127) to an RGB colour for the DS5 lightbar.
std::array<std::uint8_t, 3> program_led_color(int program) {
  
  const auto family = std::clamp(program, 0, 127) / 8;
  const float hue = static_cast<float>(family) / 16.0F; // 0..1
  const float h6 = hue * 6.0F;
  const float frac = h6 - std::floor(h6);
  const auto sector = static_cast<int>(h6) % 6;
  float r{}, g{}, b{};
  switch (sector) {
    case 0: r=1; g=frac;   b=0;      break;
    case 1: r=1-frac; g=1; b=0;      break;
    case 2: r=0; g=1;      b=frac;   break;
    case 3: r=0; g=1-frac; b=1;      break;
    case 4: r=frac; g=0;   b=1;      break;
    default: r=1; g=0;     b=1-frac; break;
  }
  return {
    static_cast<std::uint8_t>(r * 255.0F),
    static_cast<std::uint8_t>(g * 255.0F),
    static_cast<std::uint8_t>(b * 255.0F),
  };
}

std::array<std::uint8_t, 3> sequencer_led_color(int program) {
  const auto base = program_led_color(program);
  const auto max_channel = std::max({base[0], base[1], base[2]});
  if (max_channel == 0) {
    return {255, 255, 255};
  }

  auto saturate = [max_channel](std::uint8_t value) {
    const auto normalized =
        static_cast<float>(value) / static_cast<float>(max_channel);
    const auto shaped = std::pow(normalized, 1.75F);
    return static_cast<std::uint8_t>(
        std::clamp(static_cast<int>(std::lround(shaped * 255.0F)), 0, 255));
  };

  return {saturate(base[0]), saturate(base[1]), saturate(base[2])};
}

std::vector<std::string> scan_soundfonts() {
  namespace fs = std::filesystem;
  std::vector<std::string> result;
  std::vector<std::string> dirs = {
      (fs::current_path() / "soundfonts").string(),
      "/usr/share/sounds/sf2",
      "/usr/share/soundfonts",
  };
  if (const char *home = std::getenv("HOME"))
    dirs.push_back(std::string{home} + "/.local/share/sounds/sf2");
  for (const auto &dir : dirs) {
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
      if (entry.path().extension() == ".sf2")
        result.push_back(entry.path().string());
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

// Breath / wind controller state -------------------------------------------
struct BlowController {
  bool enabled{false};
  bool is_blowing{false};       // note currently held
  std::optional<Note> held_note{};
  float sensitivity{0.5F};      // 0..1  higher = MORE sensitive = lower threshold

  std::optional<std::chrono::steady_clock::time_point> attack_since{};
  std::optional<std::chrono::steady_clock::time_point> release_since{};
  std::chrono::steady_clock::time_point arm_until{};
  float ambient_floor{0.0F};
  bool ambient_ready{false};

  static constexpr auto attack_time = std::chrono::milliseconds{45};
  static constexpr auto release_time = std::chrono::milliseconds{90};
  static constexpr auto arm_cooldown_time = std::chrono::milliseconds{700};

  [[nodiscard]] float signal_threshold() const {
    const auto inverse = 1.0F - sensitivity;
    return 0.00035F + inverse * inverse * 0.00565F;
  }
  [[nodiscard]] float signal_level(float raw) const {
    return std::max(0.0F, raw - ambient_floor);
  }
  [[nodiscard]] float on_threshold() const {
    return ambient_floor + signal_threshold();
  }
  [[nodiscard]] float off_threshold() const {
    return ambient_floor + signal_threshold() * 0.35F;
  }
};

void run_event_loop(Gamepad &gamepad,
                    std::vector<analogno::WebPreset> sf2_presets,
                    std::string active_soundfont,
                    const std::vector<std::string> &available_soundfonts) {
  std::cout << "polyphonic MIDI controller running. press Ctrl+C or close the "
               "window to quit.\n\n";

  ControllerState state{};
  MusicMapper mapper{};
  MidiOutput midi{};
  ActiveNoteTracker active_notes{};
  AudioCapture audio_capture{};
  audio_capture.start();
  audio_capture.start_loopback();
  AudioSampler audio_sampler{};
  WebSocketServer web{};
  web.start();

  bool running = true;
  bool piano_roll_visible = true;
  bool spectrogram_visible = true;
  float wavetable_morph = 0.0F;
  float wavetable_noise = 0.0F;
  float wavetable_unison = 0.0F;
  auto last_print = std::chrono::steady_clock::now();
  auto last_web_publish = std::chrono::steady_clock::now();

  MusicalIntent last_intent{};
  analogno::AudioFeatures last_audio_features{};
  auto was_sample_record_button_active = false;
  auto current_midi_bank = int{0};
  auto current_midi_program = int{0};
  bool sampler_mode = false;

  WaveformSketch sketch{};
  Sequencer seq{};
  BlowController blow{};
  analogno::VoiceSequencer voice_seq{};
  // Touchpad swipe for seq step navigation (active when a step is armed).
  struct TouchSwipe { float prev_x{0.0f}; float prev_y{0.0f}; bool active{false}; };
  TouchSwipe tp_swipe{};

  // Ribbon controller: fingers 0-1 are independent pitched voices.
  // Each gets a dedicated MIDI channel (12, 13) so pitch/expression don't collide.
  struct RibbonFinger {
    bool active{false};
    int midi_channel{};
    int midi_note{-1};
    int active_channel{-1};
  };
  constexpr auto ribbon_base_ch = 12;
  constexpr auto ribbon_max = std::size_t{2};
  std::array<RibbonFinger, 2> ribbon{};
  ribbon[0].midi_channel = ribbon_base_ch;
  ribbon[1].midi_channel = ribbon_base_ch + 1;

  // Map X position [0,1] to a scale note over 2 octaves.
  const auto ribbon_note_for_x = [](float x, int root, int oct_off,
                                     analogno::ScaleKind sk) -> int {
    const auto sc = analogno::scale_for(sk);
    const int total = sc.size * 2;
    const int idx = std::clamp(static_cast<int>(x * static_cast<float>(total)), 0, total - 1);
    const int oct  = idx / sc.size;
    const int step = idx % sc.size;
    return std::clamp(root + oct_off * 12 + oct * 12 +
                      sc.semitones[static_cast<std::size_t>(step)], 0, 127);
  };

  const auto ribbon_channel_for_bank = [](const RibbonFinger &rf, int bank) {
    return bank == 128 ? 9 : rf.midi_channel;
  };

  const auto release_ribbon_finger = [&](RibbonFinger &rf) {
    if (rf.active && rf.midi_note >= 0 && rf.active_channel >= 0) {
      midi.apply_notes_only({{Note{.midi_note = rf.midi_note, .channel = rf.active_channel}}}, {});
      active_notes.apply(MusicalIntent{.note_offs = {{Note{.midi_note = rf.midi_note}}}});
    }
    rf.active = false;
    rf.midi_note = -1;
    rf.active_channel = -1;
  };

  using tp = std::chrono::steady_clock::time_point;
  struct LedFlash { std::array<std::uint8_t, 3> color{}; tp show_until{}; };
  std::deque<LedFlash> led_sequence{};
  constexpr auto led_slot_ms = std::chrono::milliseconds{80};

  while (running) {
    SDL_Event event{};

    while (SDL_PollEvent(&event)) {
      running = handle_event(event, state);

      // Touchpad physical click while a step is armed del note.
      if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
          event.gbutton.button == SDL_GAMEPAD_BUTTON_TOUCHPAD &&
          seq.selected_step >= 0) {
        const auto tidx = static_cast<std::size_t>(seq.active_track);
        const auto sidx = static_cast<std::size_t>(seq.selected_step);
        if (tidx < seq.tracks.size()) {
          seq.tracks[tidx].steps[sidx] = {};
          std::cout << "seq: cleared step " << seq.selected_step << " on track "
                    << seq.active_track << '\n';
        }
      }

      if (event.gtouchpad.touchpad == 0) {
        const auto fi = static_cast<std::size_t>(event.gtouchpad.finger);
        const bool l1_held = state.button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        const bool tp_click = state.button(SDL_GAMEPAD_BUTTON_TOUCHPAD);

        if (event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN) {
          if (fi == 0 && seq.selected_step >= 0 && !l1_held && !tp_click) {
            // Seq armed, finger 0 → swipe-to-navigate.
            tp_swipe = { event.gtouchpad.x, event.gtouchpad.y, true };
          } else if (fi == 0 && l1_held && !tp_click) {
            // L1 + touch → waveform draw.
            if (!sketch.active) { sketch.active = true; sketch.points.clear(); }
            sketch.points.emplace_back(event.gtouchpad.x, event.gtouchpad.y);
          } else if (!l1_held && !tp_click && fi < ribbon_max) {
            // Bare touch → ribbon controller (any finger).
            const int note = ribbon_note_for_x(event.gtouchpad.x,
                last_intent.root_midi_note, last_intent.octave_offset, last_intent.scale);
            const int vel = std::clamp(
                static_cast<int>((1.0f - event.gtouchpad.y) * 100.0f + 27.0f), 1, 127);
            auto &rf = ribbon[fi];
            release_ribbon_finger(rf);
            rf.active   = true;
            rf.midi_note = note;
            rf.active_channel = ribbon_channel_for_bank(rf, current_midi_bank);
            midi.program_change(current_midi_program, current_midi_bank, rf.active_channel);
            midi.apply_notes_only({}, {{Note{.midi_note = note, .velocity = vel, .channel = rf.active_channel}}});
            active_notes.apply(MusicalIntent{.note_ons = {{Note{.midi_note = note, .velocity = vel}}}});
          }
        } else if (event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION) {
          if (fi == 0 && tp_swipe.active) {
            constexpr float step_threshold  = 0.10f;
            constexpr float track_threshold = 0.12f;
            const float dx = event.gtouchpad.x - tp_swipe.prev_x;
            const float dy = event.gtouchpad.y - tp_swipe.prev_y;
            if (std::abs(dx) >= std::abs(dy)) {
              if (dx > step_threshold) {
                const auto length = active_track_loop_length(seq);
                seq.selected_step = (seq.selected_step + 1) % length;
                tp_swipe.prev_x = event.gtouchpad.x; tp_swipe.prev_y = event.gtouchpad.y;
              } else if (dx < -step_threshold) {
                const auto length = active_track_loop_length(seq);
                seq.selected_step =
                    (seq.selected_step - 1 + length) % length;
                tp_swipe.prev_x = event.gtouchpad.x; tp_swipe.prev_y = event.gtouchpad.y;
              }
            } else {
              if (dy > track_threshold) {
                seq.active_track = std::min(seq.active_track + 1,
                    static_cast<int>(seq.tracks.size()) - 1);
                tp_swipe.prev_x = event.gtouchpad.x; tp_swipe.prev_y = event.gtouchpad.y;
              } else if (dy < -track_threshold) {
                seq.active_track = std::max(seq.active_track - 1, 0);
                tp_swipe.prev_x = event.gtouchpad.x; tp_swipe.prev_y = event.gtouchpad.y;
              }
            }
          } else if (fi == 0 && sketch.active) {
            sketch.points.emplace_back(event.gtouchpad.x, event.gtouchpad.y);
          } else if (fi < ribbon_max && ribbon[fi].active) {
            const int new_note = ribbon_note_for_x(event.gtouchpad.x,
                last_intent.root_midi_note, last_intent.octave_offset, last_intent.scale);
            auto &rf = ribbon[fi];
            if (new_note != rf.midi_note) {
              const int vel = std::clamp(
                  static_cast<int>((1.0f - event.gtouchpad.y) * 100.0f + 27.0f), 1, 127);
              midi.apply_notes_only(
                  {{Note{.midi_note = rf.midi_note, .channel = rf.active_channel}}},
                  {{Note{.midi_note = new_note, .velocity = vel, .channel = rf.active_channel}}});
              active_notes.apply(MusicalIntent{
                  .note_ons  = {{Note{.midi_note = new_note, .velocity = vel}}},
                  .note_offs = {{Note{.midi_note = rf.midi_note}}}});
              rf.midi_note = new_note;
            }
          }
        } else if (event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP) {
          if (fi == 0 && tp_swipe.active) {
            tp_swipe.active = false;
          } else if (fi == 0 && sketch.active) {
            sketch.active = false;
            auto wavetable = build_wavetable(sketch.points, wavetable_length);
            if (!wavetable.empty()) {
              sketch.committed = build_wavetable(sketch.points, 128);
              sketch.committed_points = sketch.points;
              std::cout << "wavetable drawn: " << sketch.points.size() << " points\n";
              audio_sampler.set_wavetable(std::move(wavetable));
              sampler_mode = true;
            }
            sketch.points.clear();
          } else if (fi < ribbon_max && ribbon[fi].active) {
            auto &rf = ribbon[fi];
            release_ribbon_finger(rf);
          }
        }
      }
    }

    if (web.consume_panic_requested()) {
      MusicalIntent panic{};
      panic.note_off_all = true;
      midi.apply(panic);
      audio_sampler.stop_all();
      active_notes.apply(panic);
      last_intent = panic;
      for (auto &rf : ribbon) {
        rf.active = false;
        rf.midi_note = -1;
        rf.active_channel = -1;
      }
      std::cout << "panic from web UI\n";
    }

    if (const auto capture_request = web.consume_capture_device_request()) {
      audio_capture.stop();

      if (*capture_request >= 0) {
        audio_capture.start(static_cast<std::uint32_t>(*capture_request));
      } else {
        audio_capture.start();
      }
    }

    if (const auto trim_request = web.consume_sample_trim_request()) {
      audio_sampler.set_trim(trim_request->start, trim_request->end);
    }

    if (const auto bank_request = web.consume_active_bank_request()) {
      audio_sampler.stop_all();
      audio_sampler.set_active_bank(*bank_request);
      sampler_mode = audio_sampler.has_sample();
      std::cout << "active sample bank: " << *bank_request
                << " sampler_mode=" << (sampler_mode ? "on" : "off") << '\n';
    }

    if (const auto patch = web.consume_patch_request()) {
      sampler_mode = false;
      audio_sampler.stop_all();
      for (auto &rf : ribbon)
        release_ribbon_finger(rf);
      current_midi_bank = patch->bank;
      current_midi_program = patch->program;
      const auto at = seq.active_track;
      if (at >= 0 && static_cast<std::size_t>(at) < seq.tracks.size()) {
        auto &active = seq.tracks[static_cast<std::size_t>(at)];
        active.midi_program = current_midi_program;
        active.midi_bank    = current_midi_bank;
        const auto ch = active.midi_channel;
        midi.program_change(current_midi_program, current_midi_bank, ch);
        std::cout << "program change: bank=" << static_cast<int>(current_midi_bank)
                  << " program=" << static_cast<int>(current_midi_program)
                  << " ch=" << ch << '\n';
      }
      // Keep ribbon channels in sync with the active instrument.
      for (auto &rf : ribbon)
        midi.program_change(current_midi_program, current_midi_bank,
                            ribbon_channel_for_bank(rf, current_midi_bank));
    }

    if (auto sf_req = web.consume_soundfont_request()) {
      sampler_mode = false;
      audio_sampler.stop_all();
      for (auto &rf : ribbon)
        release_ribbon_finger(rf);
      active_soundfont = std::move(*sf_req);
      sf2_presets = analogno::read_sf2_presets(active_soundfont);
      if (!sf2_presets.empty()) {
        current_midi_bank = sf2_presets.front().bank;
        current_midi_program = sf2_presets.front().program;
        const auto at = seq.active_track;
        if (at >= 0 && static_cast<std::size_t>(at) < seq.tracks.size()) {
          auto &active = seq.tracks[static_cast<std::size_t>(at)];
          active.midi_bank = current_midi_bank;
          active.midi_program = current_midi_program;
          midi.program_change(current_midi_program, current_midi_bank,
                              active.midi_channel);
        }
        for (auto &rf : ribbon) {
          midi.program_change(current_midi_program, current_midi_bank,
                              ribbon_channel_for_bank(rf, current_midi_bank));
        }
      }
      std::cout << "soundfont presets updated from: " << active_soundfont
                << " (" << sf2_presets.size() << " presets)\n";
    }

    if (const auto blow_req = web.consume_blow_mode_request()) {
      if (!*blow_req && blow.enabled && blow.held_note) {
        midi.apply_notes_only({*blow.held_note}, {});
        active_notes.apply(MusicalIntent{.note_offs = {*blow.held_note}});
        blow.held_note.reset();
        blow.is_blowing = false;
      }
      blow.enabled = *blow_req;
      if (blow.enabled) {
        // Ignore mic briefly so UI-click noise cannot fire a note, and use
        // that window to learn the current room/controller noise floor.
        const auto now = std::chrono::steady_clock::now();
        blow.arm_until     = now + BlowController::arm_cooldown_time;
        blow.attack_since.reset();
        blow.release_since.reset();
        blow.ambient_floor = 0.0F;
        blow.ambient_ready = false;
        blow.is_blowing    = false;
        blow.held_note.reset();
      }
      std::cout << "blow mode: " << (blow.enabled ? "on" : "off") << '\n';
    }

    if (const auto sens = web.consume_blow_sensitivity_request()) {
      blow.sensitivity = *sens;
      std::cout << "blow sensitivity: " << blow.sensitivity << '\n';
    }

    if (const auto cfg = web.consume_voice_seq_config()) {
      voice_seq.configure({
          .enabled = cfg->enabled,
          .mode = voice_mode_from_string(cfg->mode),
          .snap_to_scale = cfg->snap_to_scale,
          .sensitivity = cfg->sensitivity,
          .timing_offset_ms = cfg->timing_offset_ms,
      });

      const auto was_recording = voice_seq.status().recording;
      if (cfg->recording && !was_recording) {
        voice_seq.start_recording(seq.bpm, active_track_loop_length(seq),
                                  seq.step_division);
      } else if (!cfg->recording && was_recording) {
        if (auto pattern = voice_seq.stop_recording(last_intent.root_midi_note,
                                                    last_intent.octave_offset,
                                                    last_intent.scale)) {
          apply_voice_pattern_to_seq(seq, *pattern);
          std::cout << "voice seq: quantized " << pattern->segment_count
                    << " segments to track " << seq.active_track << '\n';
        }
      }

      std::cout << "voice seq: " << (cfg->enabled ? "on" : "off")
                << " mode=" << cfg->mode
                << " snap=" << (cfg->snap_to_scale ? "scale" : "chromatic")
                << " recording=" << (cfg->recording ? "on" : "off")
                << " sensitivity=" << cfg->sensitivity << '\n';
    }

    if (auto wt = web.consume_wavetable_request()) {
      sketch.committed = wt->samples;
      // Resample from the UI's resolution to wavetable_length via linear interp.
      const auto src_n = wt->samples.size();
      std::vector<float> full(wavetable_length);
      for (std::size_t i = 0; i < wavetable_length; ++i) {
        const float src = static_cast<float>(i) *
                          static_cast<float>(src_n - 1) /
                          static_cast<float>(wavetable_length - 1);
        const auto lo = static_cast<std::size_t>(src);
        const auto hi = std::min(lo + 1, src_n - 1);
        const float t = src - static_cast<float>(lo);
        full[i] = wt->samples[lo] + t * (wt->samples[hi] - wt->samples[lo]);
      }

      std::vector<float> morph_full{};
      if (wt->morph_samples.size() >= 2) {
        const auto morph_src_n = wt->morph_samples.size();
        morph_full.resize(wavetable_length);
        for (std::size_t i = 0; i < wavetable_length; ++i) {
          const float src = static_cast<float>(i) *
                            static_cast<float>(morph_src_n - 1) /
                            static_cast<float>(wavetable_length - 1);
          const auto lo = static_cast<std::size_t>(src);
          const auto hi = std::min(lo + 1, morph_src_n - 1);
          const float t = src - static_cast<float>(lo);
          morph_full[i] = wt->morph_samples[lo] +
                          t * (wt->morph_samples[hi] - wt->morph_samples[lo]);
        }
      }

      audio_sampler.set_wavetable(std::move(full), std::move(morph_full));
      sampler_mode = true;
      std::cout << "wavetable set from UI; sampler mode on\n";
    }

    if (auto controls = web.consume_wavetable_controls()) {
      wavetable_morph = controls->morph;
      wavetable_noise = controls->noise;
      wavetable_unison = controls->unison;
      audio_sampler.set_wavetable_controls(wavetable_morph, wavetable_noise,
                                           wavetable_unison);
    }

    // Sequencer commands
    if (web.consume_seq_play()) {
      seq.playing = true;
      seq.current_step = -1;
      seq.playhead_step = -1;
      seq.step_start = std::chrono::steady_clock::now();
      std::cout << "seq: play\n";
    }
    if (web.consume_seq_stop()) {
      seq.playing = false;
      for (auto &track : seq.tracks) {
        if (track.pending_note_off.has_value()) {
          midi.apply_notes_only({*track.pending_note_off}, {});
          track.pending_note_off.reset();
        }
      }
      std::cout << "seq: stop\n";
    }
    if (auto cfg = web.consume_seq_config()) {
      seq.bpm      = cfg->bpm;
      seq.gate_pct = cfg->gate_pct;
      seq.step_division = sequencer_step_division(cfg->step_division);
      resize_sequencer_steps(seq, cfg->step_count);
      if (!cfg->tracks.empty()) {
        seq.tracks.resize(cfg->tracks.size());
        for (std::size_t t = 0; t < cfg->tracks.size(); ++t) {
          seq.tracks[t].steps.resize(static_cast<std::size_t>(seq.step_count));
          // Only adopt midi_channel from config if explicitly provided (>= 0).
          if (cfg->tracks[t].midi_channel >= 0)
            seq.tracks[t].midi_channel = cfg->tracks[t].midi_channel;
          seq.tracks[t].midi_program = cfg->tracks[t].midi_program;
          seq.tracks[t].midi_bank    = cfg->tracks[t].midi_bank;
          seq.tracks[t].loop_length  =
              std::clamp(cfg->tracks[t].loop_length, 1, seq.step_count);
          seq.tracks[t].muted        = cfg->tracks[t].muted;
          const auto n = std::min(seq.tracks[t].steps.size(),
                                  cfg->tracks[t].steps.size());
          for (std::size_t i = 0; i < n; ++i) {
            const auto &sc = cfg->tracks[t].steps[i];
            seq.tracks[t].steps[i] = {sc.active, sc.tie, sc.degree,
                                      sc.velocity, sc.midi_note};
          }
        }
        seq.active_track = std::clamp(seq.active_track, 0,
            static_cast<int>(seq.tracks.size()) - 1);
        if (seq.selected_step >= active_track_loop_length(seq)) {
          seq.selected_step = -1;
        }
      }
    }

    if (const auto sel = web.consume_seq_select_step()) {
      seq.selected_step =
          *sel < 0 ? -1 : std::clamp(*sel, 0, active_track_loop_length(seq) - 1);
      if (*sel >= 0) std::cout << "seq: arm step " << *sel << '\n';
      else           std::cout << "seq: disarm\n";
    }

    if (const auto trk = web.consume_seq_select_track()) {
      seq.active_track  = std::clamp(*trk, 0, static_cast<int>(seq.tracks.size()) - 1);
      seq.selected_step = -1;
      std::cout << "seq: active track " << seq.active_track << '\n';
    }

    if (web.consume_seq_add_track()) {
      if (static_cast<int>(seq.tracks.size()) < Sequencer::max_tracks) {
        // Assign the lowest MIDI channel not already in use.
        std::vector<bool> used(16, false);
        for (const auto &t : seq.tracks)
          if (t.midi_channel >= 0 && t.midi_channel < 16)
            used[static_cast<std::size_t>(t.midi_channel)] = true;
        int new_ch = 0;
        while (new_ch < 16 && used[static_cast<std::size_t>(new_ch)]) ++new_ch;
        SeqTrack new_track{};
        new_track.midi_channel = new_ch < 16 ? new_ch : 0;
        new_track.loop_length = seq.step_count;
        new_track.steps.resize(static_cast<std::size_t>(seq.step_count));
        seq.tracks.push_back(new_track);
        std::cout << "seq: added track on ch " << new_track.midi_channel << '\n';
      }
    }

    if (const auto rm = web.consume_seq_remove_track()) {
      const auto idx = static_cast<std::size_t>(*rm);
      if (seq.tracks.size() > 1 && idx < seq.tracks.size()) {
        if (seq.tracks[idx].pending_note_off) {
          midi.apply_notes_only({*seq.tracks[idx].pending_note_off}, {});
        }
        seq.tracks.erase(seq.tracks.begin() + static_cast<std::ptrdiff_t>(idx));
        seq.active_track = std::clamp(seq.active_track, 0,
            static_cast<int>(seq.tracks.size()) - 1);
        seq.selected_step = -1;
        // Resend program changes so MIDI channels still have correct programs.
        for (const auto &t : seq.tracks)
          midi.program_change(t.midi_program, t.midi_bank, t.midi_channel);
        std::cout << "seq: removed track " << idx << '\n';
      }
    }

    if (const auto save_request = web.consume_save_sample_request()) {
      const auto bank = *save_request;
      const char *home_env = std::getenv("HOME");
      const auto home = std::string{home_env != nullptr ? home_env : "."};
      const auto dir = home + "/analogno-samples";
      const auto epoch_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      const auto path = dir + "/bank-" + std::to_string(bank) + "-" +
                        std::to_string(epoch_ms) + ".wav";

      if (audio_sampler.save_bank(bank, path)) {
        std::cout << "saved bank " << bank << " to " << path << '\n';
      } else {
        std::cerr << "failed to save bank " << bank << '\n';
      }
    }


    if (state.button_pressed(SDL_GAMEPAD_BUTTON_TOUCHPAD) &&
        !state.button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) {
      const auto next = (audio_sampler.active_bank() + 1) % AudioSampler::bank_count;
      audio_sampler.stop_all();
      audio_sampler.set_active_bank(next);
      sampler_mode = audio_sampler.has_sample();
      std::cout << "bank: " << next << '\n';
    }


    if (state.button_pressed(SDL_GAMEPAD_BUTTON_BACK)) {
      const auto cur = audio_sampler.active_bank();
      const auto prev = (cur == 0 ? AudioSampler::bank_count : cur) - 1;
      audio_sampler.stop_all();
      audio_sampler.set_active_bank(prev);
      sampler_mode = audio_sampler.has_sample();
      std::cout << "bank: " << prev << '\n';
    }

    if (state.button_pressed(SDL_GAMEPAD_BUTTON_RIGHT_STICK)) {
      piano_roll_visible = !piano_roll_visible;
      std::cout << "piano roll: " << (piano_roll_visible ? "on" : "off") << '\n';
    }

    if (state.button_pressed(SDL_GAMEPAD_BUTTON_LEFT_STICK)) {
      spectrogram_visible = !spectrogram_visible;
      std::cout << "spectrogram: " << (spectrogram_visible ? "on" : "off") << '\n';
    }

    // Start button: toggle sequencer play/stop.
    if (state.button_pressed(SDL_GAMEPAD_BUTTON_START)) {
      if (seq.playing) {
        seq.playing = false;
        for (auto &track : seq.tracks) {
          if (track.pending_note_off.has_value()) {
            midi.apply_notes_only({*track.pending_note_off}, {});
            track.pending_note_off.reset();
          }
        }
        std::cout << "seq: stop (controller)\n";
      } else {
        seq.playing = true;
        seq.current_step = -1;
        seq.playhead_step = -1;
        seq.step_start = std::chrono::steady_clock::now();
        std::cout << "seq: play (controller)\n";
      }
    }

    if (audio_sampler.has_sample() &&
        state.button_pressed(SDL_GAMEPAD_BUTTON_GUIDE)) {
      audio_sampler.clear_sample();
      sampler_mode = false;
      MusicalIntent panic{};
      panic.note_off_all = true;
      midi.apply(panic);
      audio_sampler.stop_all();
      active_notes.apply(panic);
      last_intent = panic;
      std::cout << "sample cleared; MIDI mode restored\n";
    }

    const auto sample_recording_active = sample_record_button_active(state);

    if (sample_recording_active && !was_sample_record_button_active) {
      audio_capture.begin_sample_recording();
      std::cout << "sampler recording started\n";
    }

    if (!sample_recording_active && was_sample_record_button_active) {
      audio_capture.end_sample_recording();
      std::cout << "sampler recording stopped\n";
    }

    was_sample_record_button_active = sample_recording_active;

    if (auto sample = audio_capture.consume_captured_sample()) {
      const auto frame_count = sample->size();
      audio_sampler.set_sample(std::move(*sample));
      sampler_mode = true;
      MusicalIntent panic{};
      panic.note_off_all = true;
      midi.apply(panic);
      audio_sampler.stop_all();
      active_notes.apply(panic);
      last_intent = panic;
      std::cout << "captured sampler sound: " << frame_count << " frames\n";
    }

    const auto audio_features = audio_capture.consume_features();
    const auto active_sampler_bank = audio_sampler.active_bank();
    const auto l2_controls_sampler_gain =
        sampler_mode && audio_sampler.has_sample() &&
        !audio_sampler.bank_is_wavetable(active_sampler_bank);
    update_sampler_controls(mapper.map_controls(state), audio_sampler,
                            seq.playing, l2_controls_sampler_gain);
    const auto should_map = state.changed_this_frame();

    // Live MIDI channel: follows the active sequencer track.
    const auto live_ch = [&] {
      const auto at = seq.active_track;
      if (at >= 0 && static_cast<std::size_t>(at) < seq.tracks.size())
        return effective_midi_channel(seq.tracks[static_cast<std::size_t>(at)]);
      return 0;
    }();

    // Voice-to-sequencer records timed note segments from the same mic stream,
    // then quantizes the completed one-bar take into steps and ties.
    if (audio_capture.is_sample_recording()) {
      static_cast<void>(audio_capture.consume_analysis_frames(8192));
    } else {
      voice_seq.process(audio_capture, last_intent.root_midi_note,
                        last_intent.octave_offset, last_intent.scale);
      const auto voice_status = voice_seq.status();
      if (voice_status.recording && voice_status.record_progress >= 1.0F) {
        if (auto pattern = voice_seq.stop_recording(last_intent.root_midi_note,
                                                    last_intent.octave_offset,
                                                    last_intent.scale)) {
          apply_voice_pattern_to_seq(seq, *pattern);
          std::cout << "voice seq: quantized " << pattern->segment_count
                    << " segments to track " << seq.active_track << '\n';
        }
      }
    }

    if (blow.enabled && !(sampler_mode && audio_sampler.has_sample())) {
      const float raw = audio_features.envelope; // raw RMS, typically 0.001..0.150
      const auto blow_now = std::chrono::steady_clock::now();

      if (blow_now < blow.arm_until) {
        if (!blow.ambient_ready) {
          blow.ambient_floor = raw;
          blow.ambient_ready = true;
        } else {
          blow.ambient_floor = blow.ambient_floor * 0.97F + raw * 0.03F;
        }
        // Drain any pre-existing tension so arm doesn't latch immediately after cooldown.
        blow.attack_since.reset();
        blow.release_since.reset();
      } else {
        const auto signal = blow.signal_level(raw);

        // Attack / release timers.
        if (signal >= blow.signal_threshold()) {
          if (!blow.attack_since.has_value()) {
            blow.attack_since = blow_now;
          }
          blow.release_since.reset();
        } else if (signal < blow.signal_threshold() * 0.35F) {
          if (!blow.release_since.has_value()) {
            blow.release_since = blow_now;
          }
          blow.attack_since.reset();
          if (!blow.is_blowing) {
            blow.ambient_floor = blow.ambient_floor * 0.995F + raw * 0.005F;
          }
        }
        // Dead zone between off_threshold and on_threshold: neither timer advances.

        if (!blow.is_blowing && blow.attack_since.has_value() &&
            blow_now - *blow.attack_since >= BlowController::attack_time) {
          // Blow started — trigger note-on.
          const float strength = std::clamp(signal / (blow.signal_threshold() * 3.0F), 0.0F, 1.0F);
          const auto vel = std::clamp(40 + static_cast<int>(strength * 87.0F), 1, 127);
          const Note note{
              .midi_note = last_intent.root_midi_note,
              .degree    = 0,
              .octave    = last_intent.octave_offset,
              .velocity  = vel,
              .channel   = live_ch,
          };
          midi.apply_notes_only({}, {note});
          blow.held_note  = note;
          active_notes.apply(MusicalIntent{.note_ons = {note}});
          blow.is_blowing = true;
          blow.attack_since.reset();
          std::cout << "blow: note-on " << note.midi_note << " vel=" << vel << '\n';
        }

        if (blow.is_blowing && blow.release_since.has_value() &&
            blow_now - *blow.release_since >= BlowController::release_time) {
          // Blow stopped — trigger note-off.
          midi.apply_notes_only({*blow.held_note}, {});
          active_notes.apply(MusicalIntent{.note_offs = {*blow.held_note}});
          blow.held_note.reset();
          blow.is_blowing = false;
          blow.release_since.reset();
          std::cout << "blow: note-off\n";
        }
      }

      // CC2 (Breath Controller) + CC11 (Expression): sent every frame for real-time dynamics.
      const float breath = std::clamp(
          blow.signal_level(raw) / (blow.signal_threshold() * 3.0F), 0.0F, 1.0F);
      MusicalIntent cc_intent{};
      cc_intent.controls = {
          .pitch_bend = last_intent.controls.pitch_bend,
          .breath     = breath,
          .expression = breath,
          .vibrato    = last_intent.controls.vibrato,
      };
      midi.set_live_channel(live_ch);
      midi.apply(cc_intent);
    }

    if (should_map) {
      auto intent = mapper.map(state);

      // Route live play to the active track's MIDI channel.
      for (auto &n : intent.note_ons)  n.channel = live_ch;
      for (auto &n : intent.note_offs) n.channel = live_ch;

      if (sampler_mode && audio_sampler.has_sample()) {
        if (intent.note_off_all) {
          audio_sampler.stop_all();
        }
        // Release sampler voices on note-off (important for looping wavetables).
        constexpr auto sampler_root_for_release = 48;
        for (const auto &note : intent.note_offs) {
          const auto semitones = note.midi_note - sampler_root_for_release;
          audio_sampler.release(
              std::pow(2.0F, static_cast<float>(semitones) / 12.0F));
        }
        trigger_sampler_notes(intent, audio_sampler);
        active_notes.apply(intent);
      } else if (blow.enabled) {
        // Blow mode: buttons change note legato while gate is open; the
        // breath gate alone controls the note’s lifetime.
        if (blow.is_blowing && blow.held_note && !intent.note_ons.empty()) {
          auto new_note = intent.note_ons.front();
          new_note.velocity = blow.held_note->velocity;
          midi.apply_notes_only({*blow.held_note}, {new_note});
          active_notes.apply(MusicalIntent{.note_ons = {new_note},
                                           .note_offs = {*blow.held_note}});
          blow.held_note = new_note;
        }
      } else {
        midi.set_live_channel(live_ch);
        midi.apply(intent);
        active_notes.apply(intent);
      }

      last_intent = intent;
      last_audio_features = audio_features;

      // Arm recording: write any note-on into the selected (armed) step,
      // then auto-advance the arm to the next step.
      if (seq.selected_step >= 0 && !intent.note_ons.empty()) {
        const auto &n = intent.note_ons[0];
        const auto tidx = static_cast<std::size_t>(seq.active_track);
        const auto sidx = static_cast<std::size_t>(seq.selected_step);
        seq.tracks[tidx].steps[sidx] = {true, false, n.degree, n.velocity,
                                        n.midi_note};
        seq.selected_step =
            (seq.selected_step + 1) % active_track_loop_length(seq);
      }

      const auto now = std::chrono::steady_clock::now();

      if (has_note_event(intent)) {
        print_intent(intent);
        last_print = now;
      }

      state.clear_frame_edges();
    }

    // Sequencer tick — runs every frame independent of controller changes
    {
      const auto tick = tick_sequencer(seq, last_intent);
      if (!tick.note_ons.empty() || !tick.note_offs.empty()) {
        if (sampler_mode && audio_sampler.has_sample()) {
          constexpr auto sampler_root = 48;
          for (const auto &note : tick.note_offs) {
            audio_sampler.release(std::pow(2.0F,
                static_cast<float>(note.midi_note - sampler_root) / 12.0F));
          }
          MusicalIntent seq_intent{};
          seq_intent.note_ons = tick.note_ons;
          trigger_sampler_notes(seq_intent, audio_sampler);
        } else {
          // apply_notes_only: no CC/pitch-bend side-effects that would override
          // the live player's expression, filter, modulation, etc.
          midi.apply_notes_only(tick.note_offs, tick.note_ons);
        }
      }
      // Lightbar: enqueue one colour flash per note_on, preserving order.
      if (!tick.note_ons.empty()) {
        // Build channel->program map for colour lookup.
        std::array<int, 16> ch_prog{};
        for (const auto &track : seq.tracks) {
          const auto ch = std::clamp(effective_midi_channel(track), 0, 15);
          ch_prog[static_cast<std::size_t>(ch)] = track.midi_program;
        }
        for (const auto &note : tick.note_ons) {
          const auto ch = std::clamp(note.channel, 0, 15);
          // Schedule: starts after all previously queued entries finish.
          const auto starts_at = led_sequence.empty()
              ? std::chrono::steady_clock::now()
              : led_sequence.back().show_until;
          led_sequence.push_back({sequencer_led_color(ch_prog[static_cast<std::size_t>(ch)]),
                                   starts_at + led_slot_ms});
        }
      }
    }

    // Sequential microflash: show front entry until its time expires, then advance.
    if (seq.playing) {
      const auto led_now = std::chrono::steady_clock::now();
      // Pop any expired entries.
      while (!led_sequence.empty() && led_now >= led_sequence.front().show_until) {
        led_sequence.pop_front();
      }
      if (!led_sequence.empty()) {
        const auto &front = led_sequence.front();
        gamepad.set_led(front.color[0], front.color[1], front.color[2]);
      } else {
        gamepad.set_led(10, 10, 14);
      }
    } else {
      if (!led_sequence.empty()) {
        led_sequence.clear();
        gamepad.set_led(0, 0, 0);
      }
    }

    const auto now = std::chrono::steady_clock::now();

    if (now - last_web_publish >= std::chrono::milliseconds{100}) {
      if (!should_map) {
        last_audio_features = audio_features;
      }

      static int spec_counter = 0;
      const auto spec_data = (spectrogram_visible && (++spec_counter % 3 == 0))
                                 ? audio_capture.spec_samples()
                                 : std::vector<float>{};

      web.publish(make_web_state(state, last_intent, active_notes,
                                 audio_capture, audio_sampler,
                                 last_audio_features,
                                 current_midi_program, current_midi_bank,
                                 sketch, seq, sf2_presets,
                                 available_soundfonts, active_soundfont,
                                 piano_roll_visible,
                                 spectrogram_visible,
                                 blow.enabled,
                                 wavetable_morph,
                                 wavetable_noise,
                                 wavetable_unison,
                                 blow.sensitivity,
                                 blow.is_blowing,
                                 // blow_level: breath signal above ambient relative to trigger threshold.
                                 std::clamp(blow.signal_level(audio_features.envelope) /
                                                blow.signal_threshold(),
                                            0.0F, 2.0F),
                                 voice_seq.status(),
                                 spec_data));
      last_web_publish = now;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

} // namespace

int main(int argc, char *argv[]) {
  std::string soundfont_path{};
  for (int i = 1; i < argc; ++i) {
    if (std::string_view{argv[i]} == "--soundfont" && i + 1 < argc) {
      soundfont_path = argv[++i];
    }
  }

  auto sf2_presets = soundfont_path.empty()
      ? std::vector<analogno::WebPreset>{}
      : analogno::read_sf2_presets(soundfont_path);

  if (!sf2_presets.empty()) {
    std::cout << "loaded " << sf2_presets.size() << " presets from " << soundfont_path << '\n';
  }

  const auto available_soundfonts = scan_soundfonts();

  const Sdl sdl{};

  auto gamepad = open_first_gamepad();

  if (!gamepad.has_value()) {
    return EXIT_FAILURE;
  }

  print_capabilities(*gamepad);
  enable_motion_sensors(*gamepad);
  run_event_loop(*gamepad, std::move(sf2_presets), std::move(soundfont_path),
                 available_soundfonts);

  return EXIT_SUCCESS;
}
