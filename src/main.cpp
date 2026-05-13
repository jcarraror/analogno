#include "audio_capture.hpp"
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

#include <array>
#include <chrono>
#include <cstddef>
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
  auto operator=(const Sdl &) -> Sdl & = delete;
  Sdl(Sdl &&) = delete;
  auto operator=(Sdl &&) -> Sdl & = delete;
};

class ActiveNoteTracker final {
public:
  auto apply(const MusicalIntent &intent) -> void {
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

  [[nodiscard]] auto notes() const -> std::vector<int> {
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

  static auto valid(int note) -> bool { return note >= 0 && note < 128; }
};

auto open_first_gamepad() -> std::optional<Gamepad> {
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

auto print_capabilities(const Gamepad &gamepad) -> void {
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

auto enable_motion_sensors(const Gamepad &gamepad) -> void {
  gamepad.enable_sensor(SDL_SENSOR_GYRO, "gyro");
  gamepad.enable_sensor(SDL_SENSOR_ACCEL, "accelerometer");
  std::cout << '\n';
}

auto print_note_on(const analogno::Note &note) -> void {
  std::cout << "note_on"
            << " midi=" << note.midi_note << " degree=" << note.degree
            << " octave=" << note.octave << '\n';
}

auto print_note_off(const analogno::Note &note) -> void {
  std::cout << "note_off"
            << " midi=" << note.midi_note << " degree=" << note.degree
            << " octave=" << note.octave << '\n';
}

auto has_note_event(const MusicalIntent &intent) -> bool {
  return !intent.note_ons.empty() || !intent.note_offs.empty() ||
         intent.note_off_all;
}

auto print_intent(const MusicalIntent &intent) -> void {
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

auto handle_event(const SDL_Event &event, ControllerState &state) -> bool {
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

auto make_web_state(const ControllerState &controller,
                    const MusicalIntent &intent,
                    const ActiveNoteTracker &active_notes,
                    const AudioCapture &audio_capture)
    -> analogno::WebRuntimeState {
  std::vector<analogno::WebCaptureDevice> capture_devices{};

  for (const auto &device : audio_capture.devices()) {
    capture_devices.push_back(analogno::WebCaptureDevice{
        .index = device.index,
        .name = device.name,
        .is_default = device.is_default,
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
          },
      .audio =
          analogno::WebAudioState{
              .devices = std::move(capture_devices),
              .selected_device_index = audio_capture.selected_device_index(),
              .capture_running = audio_capture.is_running(),
              .capture_device = audio_capture.selected_device_name(),
              .mic_level = audio_capture.level(),
              .waveform = audio_capture.waveform(),
          },
  };
}

auto run_event_loop() -> void {
  std::cout << "polyphonic MIDI controller running. press Ctrl+C or close the "
               "window to quit.\n\n";

  ControllerState state{};
  MusicMapper mapper{};
  MidiOutput midi{};
  ActiveNoteTracker active_notes{};
  AudioCapture audio_capture{};
  audio_capture.start();
  WebSocketServer web{};
  web.start();

  bool running = true;
  auto last_print = std::chrono::steady_clock::now();
  auto last_web_publish = std::chrono::steady_clock::now();

  MusicalIntent last_intent{};

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

    if (state.changed_this_frame()) {
      const auto intent = mapper.map(state);
      midi.apply(intent);
      active_notes.apply(intent);
      last_intent = intent;

      const auto now = std::chrono::steady_clock::now();

      if (has_note_event(intent)) {
        print_intent(intent);
        last_print = now;
      }

      state.clear_frame_edges();
    }

    const auto now = std::chrono::steady_clock::now();

    if (now - last_web_publish >= std::chrono::milliseconds{100}) {
      web.publish(
          make_web_state(state, last_intent, active_notes, audio_capture));
      last_web_publish = now;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

} // namespace

auto main(int, char **) -> int {
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
