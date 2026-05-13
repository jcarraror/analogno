#include "audio_capture.hpp"
#include "audio_sampler.hpp"
#include "controller_state.hpp"
#include "midi_output.hpp"
#include "music_mapper.hpp"
#include "music_types.hpp"
#include "sdl_check.hpp"
#include "sdl_gamepad.hpp"
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
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>
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
                             AudioSampler &sampler) {
  sampler.set_gain(controls.expression);
  sampler.set_pitch_controls(controls.pitch_bend, controls.vibrato);
}

bool sample_record_button_active(const ControllerState &controller) {
  return controller.button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) &&
         controller.button(SDL_GAMEPAD_BUTTON_TOUCHPAD);
}

analogno::WebRuntimeState
make_web_state(const ControllerState &controller, const MusicalIntent &intent,
               const ActiveNoteTracker &active_notes,
               const AudioCapture &audio_capture,
               const AudioSampler &audio_sampler,
               const analogno::AudioFeatures &audio_features,
               std::uint8_t midi_program,
               std::uint8_t midi_bank) {
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
              .banks = std::move(sample_banks),
              .active_bank = audio_sampler.active_bank(),
          },
  };
}

void run_event_loop() {
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
  auto current_midi_bank = std::uint8_t{0};
  auto current_midi_program = std::uint8_t{0};

  while (running) {
    SDL_Event event{};

    while (SDL_PollEvent(&event)) {
      running = handle_event(event, state);
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
      midi.program_change(current_midi_program, current_midi_bank);
      std::cout << "program change: bank=" << static_cast<int>(current_midi_bank)
                << " program=" << static_cast<int>(current_midi_program) << '\n';
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

    
    if (state.button_pressed(SDL_GAMEPAD_BUTTON_START)) {
      const auto cur = audio_sampler.active_bank();
      const auto prev = (cur == 0 ? AudioSampler::bank_count : cur) - 1;
      audio_sampler.set_active_bank(prev);
      std::cout << "bank: " << prev << '\n';
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
    update_sampler_controls(mapper.map_controls(state), audio_sampler);
    const auto should_map = state.changed_this_frame();

    if (should_map) {
      auto intent = mapper.map(state);

      if (audio_sampler.has_sample()) {
        trigger_sampler_notes(intent, audio_sampler);
        active_notes.apply(intent);
      } else {
        midi.apply(intent);
        active_notes.apply(intent);
      }

      last_intent = intent;
      last_audio_features = audio_features;

      const auto now = std::chrono::steady_clock::now();

      if (has_note_event(intent)) {
        print_intent(intent);
        last_print = now;
      }

      state.clear_frame_edges();
    }

    const auto now = std::chrono::steady_clock::now();

    if (now - last_web_publish >= std::chrono::milliseconds{100}) {
      if (!should_map) {
        last_audio_features = audio_features;
      }

      web.publish(make_web_state(state, last_intent, active_notes,
                                 audio_capture, audio_sampler,
                                 last_audio_features,
                                 current_midi_program, current_midi_bank));
      last_web_publish = now;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

} // namespace

int main(int, char **) {
  const Sdl sdl{};

  auto gamepad = open_first_gamepad();

  if (!gamepad.has_value()) {
    return EXIT_FAILURE;
  }

  print_capabilities(*gamepad);
  enable_motion_sensors(*gamepad);
  run_event_loop();

  return EXIT_SUCCESS;
}
