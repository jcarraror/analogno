#include "music_mapper.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

namespace analogno {
namespace {

constexpr auto midi_min = 0;
constexpr auto midi_max = 127;

struct PlayableButton final {
  SDL_GamepadButton button{};
  int degree{};
  std::size_t slot{};
};

constexpr auto playable_buttons = std::array{
    PlayableButton{.button = SDL_GAMEPAD_BUTTON_SOUTH, .degree = 0, .slot = 0},
    PlayableButton{.button = SDL_GAMEPAD_BUTTON_EAST, .degree = 1, .slot = 1},
    PlayableButton{.button = SDL_GAMEPAD_BUTTON_WEST, .degree = 2, .slot = 2},
    PlayableButton{.button = SDL_GAMEPAD_BUTTON_NORTH, .degree = 3, .slot = 3},
};

int clamp_midi(int note) {
  return std::clamp(note, midi_min, midi_max);
}

int clamp_octave(int octave) { return std::clamp(octave, -4, 4); }

ScaleKind next_scale(ScaleKind scale) {
  switch (scale) {
  case ScaleKind::minor_pentatonic:
    return ScaleKind::major;
  case ScaleKind::major:
    return ScaleKind::natural_minor;
  case ScaleKind::natural_minor:
    return ScaleKind::dorian;
  case ScaleKind::dorian:
    return ScaleKind::phrygian;
  case ScaleKind::phrygian:
    return ScaleKind::chromatic;
  case ScaleKind::chromatic:
    return ScaleKind::minor_pentatonic;
  }

  return ScaleKind::minor_pentatonic;
}

ScaleKind previous_scale(ScaleKind scale) {
  switch (scale) {
  case ScaleKind::minor_pentatonic:
    return ScaleKind::chromatic;
  case ScaleKind::major:
    return ScaleKind::minor_pentatonic;
  case ScaleKind::natural_minor:
    return ScaleKind::major;
  case ScaleKind::dorian:
    return ScaleKind::natural_minor;
  case ScaleKind::phrygian:
    return ScaleKind::dorian;
  case ScaleKind::chromatic:
    return ScaleKind::phrygian;
  }

  return ScaleKind::minor_pentatonic;
}

float positive_from_axis(float value) {
  return std::clamp((value + 1.0F) * 0.5F, 0.0F, 1.0F);
}

} // namespace

Scale scale_for(ScaleKind kind) {
  switch (kind) {
  case ScaleKind::minor_pentatonic:
    return Scale{
        .kind = kind,
        .name = "minor_pentatonic",
        .semitones = {0, 3, 5, 7, 10},
        .size = 5,
    };

  case ScaleKind::major:
    return Scale{
        .kind = kind,
        .name = "major",
        .semitones = {0, 2, 4, 5, 7, 9, 11},
        .size = 7,
    };

  case ScaleKind::natural_minor:
    return Scale{
        .kind = kind,
        .name = "natural_minor",
        .semitones = {0, 2, 3, 5, 7, 8, 10},
        .size = 7,
    };

  case ScaleKind::dorian:
    return Scale{
        .kind = kind,
        .name = "dorian",
        .semitones = {0, 2, 3, 5, 7, 9, 10},
        .size = 7,
    };

  case ScaleKind::phrygian:
    return Scale{
        .kind = kind,
        .name = "phrygian",
        .semitones = {0, 1, 3, 5, 7, 8, 10},
        .size = 7,
    };

  case ScaleKind::chromatic:
    return Scale{
        .kind = kind,
        .name = "chromatic",
        .semitones = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
        .size = 12,
    };
  }

  return scale_for(ScaleKind::minor_pentatonic);
}

std::string_view scale_name(ScaleKind kind) {
  return scale_for(kind).name;
}

MusicalIntent MusicMapper::map(const ControllerState &controller) {
  MusicalIntent intent{
      .root_midi_note = root_midi_note_,
      .octave_offset = octave_offset_,
      .scale = scale_,
  };

  update_mode_buttons(controller, intent);
  map_note_buttons(controller, intent);

  intent.controls = map_controls(controller);

  return intent;
}

ContinuousControls
MusicMapper::map_controls(const ControllerState &controller) const {
  const auto gyro_vibrato =
      controller.has_gyro()
          ? std::clamp(controller.gyro().z * 0.12F, -0.35F, 0.35F)
          : 0.0F;

  const auto vibrato_amount = controller.left_trigger();

  const auto pitch_bend = std::clamp(
      controller.left_x() + gyro_vibrato * vibrato_amount, -1.0F, 1.0F);

  return ContinuousControls{
      .pitch_bend = pitch_bend,
      .expression = controller.left_trigger(),
      .filter_cutoff = positive_from_axis(-controller.right_y()),
      .filter_resonance = positive_from_axis(controller.right_x()),
      .modulation = controller.right_trigger(),
      .vibrato = std::abs(gyro_vibrato) * vibrato_amount,
  };
}

void MusicMapper::map_note_buttons(const ControllerState &controller,
                                   MusicalIntent &intent) {
  for (const auto playable : playable_buttons) {
    auto &active_note = active_notes_[playable.slot];

    if (controller.button_pressed(playable.button)) {
      const auto note = note_for_degree(playable.degree);
      active_note = note;
      intent.note_ons.push_back(note);
    }

    if (controller.button_released(playable.button) &&
        active_note.has_value()) {
      intent.note_offs.push_back(*active_note);
      active_note.reset();
    }
  }
}

void MusicMapper::update_mode_buttons(const ControllerState &controller,
                                      MusicalIntent &intent) {
  const auto l1 = controller.button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
  const auto r1 = controller.button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
  const auto dpad_up = controller.button(SDL_GAMEPAD_BUTTON_DPAD_UP);
  const auto dpad_down = controller.button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
  const auto dpad_left = controller.button(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
  const auto dpad_right = controller.button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
  const auto guide = controller.button(SDL_GAMEPAD_BUTTON_GUIDE);

  if (rising_edge(l1, previous_l1_)) {
    scale_ = previous_scale(scale_);
  }

  if (rising_edge(r1, previous_r1_)) {
    scale_ = next_scale(scale_);
  }

  if (rising_edge(dpad_up, previous_dpad_up_)) {
    octave_offset_ = clamp_octave(octave_offset_ + 1);
  }

  if (rising_edge(dpad_down, previous_dpad_down_)) {
    octave_offset_ = clamp_octave(octave_offset_ - 1);
  }

  if (rising_edge(dpad_left, previous_dpad_left_)) {
    root_midi_note_ = clamp_midi(root_midi_note_ - 1);
  }

  if (rising_edge(dpad_right, previous_dpad_right_)) {
    root_midi_note_ = clamp_midi(root_midi_note_ + 1);
  }

  if (rising_edge(guide, previous_guide_)) {
    intent.note_off_all = true;
    active_notes_.fill(std::nullopt);
  }

  intent.root_midi_note = root_midi_note_;
  intent.octave_offset = octave_offset_;
  intent.scale = scale_;
}

Note MusicMapper::note_for_degree(int degree) const {
  const auto scale = scale_for(scale_);
  const auto wrapped_degree = degree % scale.size;
  const auto extra_octave = degree / scale.size;
  const auto octave = octave_offset_ + extra_octave;
  const auto semitone =
      scale.semitones[static_cast<std::size_t>(wrapped_degree)];

  return Note{
      .midi_note = clamp_midi(root_midi_note_ + semitone + octave * 12),
      .degree = degree,
      .octave = octave,
      .velocity = 100,
      .channel = 0,
  };
}

bool MusicMapper::rising_edge(bool current, bool &previous) {
  const auto result = current && !previous;
  previous = current;
  return result;
}

} // namespace analogno
