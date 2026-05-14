#include "audio_capture.hpp"
#include "audio_sampler.hpp"
#include "controller_state.hpp"
#include "midi_output.hpp"
#include "music_mapper.hpp"
#include "music_types.hpp"
#include "sdl_check.hpp"
#include "sdl_gamepad.hpp"
#include "sf2_reader.hpp"
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
using analogno::WebSocketServer;


constexpr auto wavetable_length = std::size_t{367};

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
                             bool seq_playing) {
  // During sequencer playback, play at full gain so notes are heard without
  // holding any trigger. The trigger only controls gain for live play.
  const auto gain = seq_playing ? 1.0F : controls.expression;
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
  int degree{0};
  int velocity{100};
  int midi_note{-1}; // -1 = compute from root/scale/octave; >=0 = absolute pitch
};

struct SeqTrack {
  static constexpr int step_count = 16;
  int midi_channel{0}; // persists across track reordering
  int midi_program{0};
  int midi_bank{0};
  bool muted{false};
  std::array<SeqStep, step_count> steps{};
  std::optional<analogno::Note> pending_note_off{};
};

struct Sequencer {
  static constexpr int step_count = SeqTrack::step_count;
  static constexpr int max_tracks = 16;
  bool playing{false};
  int active_track{0};
  int selected_step{-1}; // step in active_track armed for controller input; -1 = none
  float bpm{120.0F};
  int gate_pct{50};
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

SeqTick tick_sequencer(Sequencer &seq, const analogno::MusicalIntent &ctx) {
  if (!seq.playing) return {};

  const auto now = std::chrono::steady_clock::now();
  using ms = std::chrono::duration<double, std::milli>;
  const auto step_dur = ms{60000.0 / static_cast<double>(seq.bpm) / 4.0};
  const auto gate_dur = step_dur * (seq.gate_pct / 100.0);
  const auto elapsed  = std::chrono::duration_cast<ms>(now - seq.step_start);

  SeqTick result{};

  // Gate close — per track
  for (auto &track : seq.tracks) {
    if (track.pending_note_off.has_value() && elapsed >= gate_dur) {
      result.note_offs.push_back(*track.pending_note_off);
      track.pending_note_off.reset();
    }
  }

  if (elapsed >= step_dur) {
    seq.current_step = (seq.current_step + 1) % Sequencer::step_count;
    seq.step_start = now;
    result.stepped = true;

    for (std::size_t t = 0; t < seq.tracks.size(); ++t) {
      auto &track = seq.tracks[t];
      // Flush any still-held note on this track (gate_pct=100 case)
      if (track.pending_note_off.has_value()) {
        result.note_offs.push_back(*track.pending_note_off);
        track.pending_note_off.reset();
      }

      const auto &step = track.steps[static_cast<std::size_t>(seq.current_step)];
      if (step.active && !track.muted) {
        const auto scale    = analogno::scale_for(ctx.scale);
        const auto wrapped  = step.degree % scale.size;
        const auto xoct     = step.degree / scale.size;
        const auto semitone = scale.semitones[static_cast<std::size_t>(wrapped)];
        const auto computed = std::clamp(
            ctx.root_midi_note + semitone + (ctx.octave_offset + xoct) * 12,
            0, 127);
        const auto midi_note = step.midi_note >= 0 ? step.midi_note : computed;

        const analogno::Note note{
            .midi_note = midi_note,
            .degree    = step.degree,
            .octave    = ctx.octave_offset + xoct,
            .velocity  = step.velocity,
            .channel   = track.midi_channel, // stored, not derived from index
        };
        result.note_ons.push_back(note);
        track.pending_note_off = note;
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
  s.current_step  = seq.current_step;
  s.gate_pct      = seq.gate_pct;
  for (const auto &track : seq.tracks) {
    analogno::WebSeqTrack wt{};
    wt.midi_channel = track.midi_channel;
    wt.midi_program = track.midi_program;
    wt.midi_bank    = track.midi_bank;
    wt.muted        = track.muted;
    wt.steps.reserve(Sequencer::step_count);
    for (const auto &step : track.steps) {
      wt.steps.push_back({step.active, step.degree, step.velocity, step.midi_note});
    }
    s.tracks.push_back(std::move(wt));
  }
  return s;
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
               const std::string &active_soundfont) {
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
      .music =
          analogno::WebMusicState{
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
          },
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
          },
      .seq = seq_web_state(seq),
      .presets = sf2_presets,
      .soundfonts = available_soundfonts,
      .active_soundfont = active_soundfont,
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

std::vector<std::string> scan_soundfonts() {
  namespace fs = std::filesystem;
  std::vector<std::string> result;
  std::vector<std::string> dirs = {
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
  AudioSampler audio_sampler{};
  WebSocketServer web{};
  web.start();

  bool running = true;
  auto last_print = std::chrono::steady_clock::now();
  auto last_web_publish = std::chrono::steady_clock::now();

  MusicalIntent last_intent{};
  analogno::AudioFeatures last_audio_features{};
  auto was_sample_record_button_active = false;
  auto current_midi_bank = int{0};
  auto current_midi_program = int{0};
  WaveformSketch sketch{};
  Sequencer seq{};
// Touchpad swipe for seq step navigation active when a step is armed
  struct TouchSwipe { float prev_x{0.0f}; float prev_y{0.0f}; bool active{false}; };
  TouchSwipe tp_swipe{};

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
          seq.tracks[tidx].steps[sidx] = {false, 0, 100, -1};
          std::cout << "seq: cleared step " << seq.selected_step << " on track "
                    << seq.active_track << '\n';
        }
      }

      if (event.gtouchpad.touchpad == 0 && event.gtouchpad.finger == 0) {
        const bool l1_held = state.button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        const bool tp_click = state.button(SDL_GAMEPAD_BUTTON_TOUCHPAD);

        if (event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN) {
          if (seq.selected_step >= 0 && !l1_held && !tp_click) {
            // Seq armed — enter swipe-to-navigate mode.
            tp_swipe = { event.gtouchpad.x, event.gtouchpad.y, true };
          } else if (!l1_held && !tp_click) {
            if (!sketch.active) {
              sketch.active = true;
              sketch.points.clear();
            }
            sketch.points.emplace_back(event.gtouchpad.x, event.gtouchpad.y);
          }
        } else if (event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION) {
          if (tp_swipe.active) {
            constexpr float step_threshold = 0.10f;
            constexpr float track_threshold = 0.12f;
            const float dx = event.gtouchpad.x - tp_swipe.prev_x;
            const float dy = event.gtouchpad.y - tp_swipe.prev_y;
            if (std::abs(dx) >= std::abs(dy)) {
              // Horizontal: step navigation.
              if (dx > step_threshold) {
                seq.selected_step = (seq.selected_step + 1) % Sequencer::step_count;
                tp_swipe.prev_x = event.gtouchpad.x;
                tp_swipe.prev_y = event.gtouchpad.y;
              } else if (dx < -step_threshold) {
                seq.selected_step = (seq.selected_step - 1 + Sequencer::step_count) % Sequencer::step_count;
                tp_swipe.prev_x = event.gtouchpad.x;
                tp_swipe.prev_y = event.gtouchpad.y;
              }
            } else {
              // Vertical: track switching.
              if (dy > track_threshold) {
                seq.active_track = std::min(seq.active_track + 1,
                    static_cast<int>(seq.tracks.size()) - 1);
                tp_swipe.prev_x = event.gtouchpad.x;
                tp_swipe.prev_y = event.gtouchpad.y;
              } else if (dy < -track_threshold) {
                seq.active_track = std::max(seq.active_track - 1, 0);
                tp_swipe.prev_x = event.gtouchpad.x;
                tp_swipe.prev_y = event.gtouchpad.y;
              }
            }
          } else if (!l1_held && !tp_click) {
            if (!sketch.active) {
              sketch.active = true;
              sketch.points.clear();
            }
            sketch.points.emplace_back(event.gtouchpad.x, event.gtouchpad.y);
          }
        } else if (event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP) {
          if (tp_swipe.active) {
            tp_swipe.active = false;
          } else if (sketch.active) {
            sketch.active = false;
            auto wavetable = build_wavetable(sketch.points, wavetable_length);
            if (!wavetable.empty()) {
              sketch.committed = build_wavetable(sketch.points, 128);
              sketch.committed_points = sketch.points;
              std::cout << "wavetable drawn: " << sketch.points.size()
                        << " points\n";
              audio_sampler.set_wavetable(std::move(wavetable));
            }
            sketch.points.clear();
          }
        }
      }
    }

    if (web.consume_panic_requested()) {
      MusicalIntent panic{};
      panic.note_off_all = true;
      midi.apply(panic);
      active_notes.apply(panic);
      last_intent = panic;
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
      audio_sampler.set_active_bank(*bank_request);
      std::cout << "active bank: " << *bank_request << '\n';
    }

    if (const auto patch = web.consume_patch_request()) {
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
    }

    if (auto sf_req = web.consume_soundfont_request()) {
      active_soundfont = std::move(*sf_req);
      sf2_presets = analogno::read_sf2_presets(active_soundfont);
      std::cout << "soundfont presets updated from: " << active_soundfont
                << " (" << sf2_presets.size() << " presets)\n";
    }

    if (auto wt = web.consume_wavetable_request()) {
      sketch.committed = *wt;
      // Resample from the UI's resolution to wavetable_length via linear interp.
      const auto src_n = wt->size();
      std::vector<float> full(wavetable_length);
      for (std::size_t i = 0; i < wavetable_length; ++i) {
        const float src = static_cast<float>(i) *
                          static_cast<float>(src_n - 1) /
                          static_cast<float>(wavetable_length - 1);
        const auto lo = static_cast<std::size_t>(src);
        const auto hi = std::min(lo + 1, src_n - 1);
        const float t = src - static_cast<float>(lo);
        full[i] = (*wt)[lo] + t * ((*wt)[hi] - (*wt)[lo]);
      }
      audio_sampler.set_wavetable(std::move(full));
      std::cout << "wavetable set from UI\n";
    }

    // Sequencer commands
    if (web.consume_seq_play()) {
      seq.playing = true;
      seq.current_step = -1;
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
      if (!cfg->tracks.empty()) {
        seq.tracks.resize(cfg->tracks.size());
        for (std::size_t t = 0; t < cfg->tracks.size(); ++t) {
          // Only adopt midi_channel from config if explicitly provided (>= 0).
          if (cfg->tracks[t].midi_channel >= 0)
            seq.tracks[t].midi_channel = cfg->tracks[t].midi_channel;
          seq.tracks[t].midi_program = cfg->tracks[t].midi_program;
          seq.tracks[t].midi_bank    = cfg->tracks[t].midi_bank;
          seq.tracks[t].muted        = cfg->tracks[t].muted;
          for (std::size_t i = 0; i < static_cast<std::size_t>(Sequencer::step_count); ++i) {
            const auto &sc = cfg->tracks[t].steps[i];
            seq.tracks[t].steps[i] = {sc.active, sc.degree, sc.velocity, sc.midi_note};
          }
        }
        seq.active_track = std::clamp(seq.active_track, 0,
            static_cast<int>(seq.tracks.size()) - 1);
      }
    }

    if (const auto sel = web.consume_seq_select_step()) {
      seq.selected_step = *sel;
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
      audio_sampler.set_active_bank(next);
      std::cout << "bank: " << next << '\n';
    }


    if (state.button_pressed(SDL_GAMEPAD_BUTTON_BACK)) {
      const auto cur = audio_sampler.active_bank();
      const auto prev = (cur == 0 ? AudioSampler::bank_count : cur) - 1;
      audio_sampler.set_active_bank(prev);
      std::cout << "bank: " << prev << '\n';
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
        seq.step_start = std::chrono::steady_clock::now();
        std::cout << "seq: play (controller)\n";
      }
    }

    if (audio_sampler.has_sample() &&
        state.button_pressed(SDL_GAMEPAD_BUTTON_GUIDE)) {
      audio_sampler.clear_sample();
      MusicalIntent panic{};
      panic.note_off_all = true;
      midi.apply(panic);
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
      MusicalIntent panic{};
      panic.note_off_all = true;
      midi.apply(panic);
      active_notes.apply(panic);
      last_intent = panic;
      std::cout << "captured sampler sound: " << frame_count << " frames\n";
    }

    const auto audio_features = audio_capture.consume_features();
    update_sampler_controls(mapper.map_controls(state), audio_sampler, seq.playing);
    const auto should_map = state.changed_this_frame();

    if (should_map) {
      auto intent = mapper.map(state);

      // Route live play to the active track's MIDI channel so the DS5 always
      // sounds like the instrument selected on that track.
      const auto live_ch = [&] {
        const auto at = seq.active_track;
        if (at >= 0 && static_cast<std::size_t>(at) < seq.tracks.size())
          return seq.tracks[static_cast<std::size_t>(at)].midi_channel;
        return 0;
      }();
      for (auto &n : intent.note_ons)  n.channel = live_ch;
      for (auto &n : intent.note_offs) n.channel = live_ch;

      if (audio_sampler.has_sample()) {
        // Release sampler voices on note-off (important for looping wavetables).
        constexpr auto sampler_root_for_release = 48;
        for (const auto &note : intent.note_offs) {
          const auto semitones = note.midi_note - sampler_root_for_release;
          audio_sampler.release(
              std::pow(2.0F, static_cast<float>(semitones) / 12.0F));
        }
        trigger_sampler_notes(intent, audio_sampler);
        active_notes.apply(intent);
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
        seq.tracks[tidx].steps[sidx] = {true, n.degree, n.velocity, n.midi_note};
        seq.selected_step = (seq.selected_step + 1) % Sequencer::step_count;
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
        if (audio_sampler.has_sample()) {
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
          const auto ch = std::clamp(track.midi_channel, 0, 15);
          ch_prog[static_cast<std::size_t>(ch)] = track.midi_program;
        }
        for (const auto &note : tick.note_ons) {
          const auto ch = std::clamp(note.channel, 0, 15);
          // Schedule: starts after all previously queued entries finish.
          const auto starts_at = led_sequence.empty()
              ? std::chrono::steady_clock::now()
              : led_sequence.back().show_until;
          led_sequence.push_back({program_led_color(ch_prog[static_cast<std::size_t>(ch)]),
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
        gamepad.set_led(0, 0, 0);
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

      web.publish(make_web_state(state, last_intent, active_notes,
                                 audio_capture, audio_sampler,
                                 last_audio_features,
                                 current_midi_program, current_midi_bank,
                                 sketch, seq, sf2_presets,
                                 available_soundfonts, active_soundfont));
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
