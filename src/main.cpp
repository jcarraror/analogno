#include "controller_state.hpp"
#include "music_mapper.hpp"
#include "music_types.hpp"
#include "sdl_check.hpp"
#include "sdl_gamepad.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_timer.h>

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <thread>

namespace {

using analogno::ControllerState;
using analogno::Gamepad;
using analogno::MusicalIntent;
using analogno::MusicMapper;

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

auto print_note(const analogno::Note &note) -> void {
  std::cout << "note_on"
            << " midi=" << note.midi_note << " degree=" << note.degree
            << " octave=" << note.octave << '\n';
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

  if (intent.note_on.has_value()) {
    print_note(*intent.note_on);
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

auto run_event_loop() -> void {
  std::cout << "musical intent monitor running. press Ctrl+C or close the "
               "window to quit.\n\n";

  ControllerState state{};
  MusicMapper mapper{};

  bool running = true;
  auto last_print = std::chrono::steady_clock::now();

  while (running) {
    SDL_Event event{};

    while (SDL_PollEvent(&event)) {
      running = handle_event(event, state);
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = now - last_print;

    if (state.changed_this_frame() &&
        elapsed >= std::chrono::milliseconds{33}) {
      const auto intent = mapper.map(state);
      print_intent(intent);

      state.clear_frame_edges();
      last_print = now;
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
