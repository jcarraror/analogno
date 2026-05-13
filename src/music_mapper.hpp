#pragma once

#include "controller_state.hpp"
#include "music_types.hpp"

namespace analogno {

class MusicMapper final {
public:
  [[nodiscard]] auto map(const ControllerState &controller) -> MusicalIntent;

private:
  int root_midi_note_{48};
  int octave_offset_{};
  ScaleKind scale_{ScaleKind::minor_pentatonic};

  bool previous_south_{};
  bool previous_east_{};
  bool previous_west_{};
  bool previous_north_{};
  bool previous_l1_{};
  bool previous_r1_{};
  bool previous_dpad_up_{};
  bool previous_dpad_down_{};
  bool previous_dpad_left_{};
  bool previous_dpad_right_{};
  bool previous_guide_{};

  [[nodiscard]] auto map_note_buttons(const ControllerState &controller)
      -> std::optional<Note>;
  auto update_mode_buttons(const ControllerState &controller,
                           MusicalIntent &intent) -> void;

  [[nodiscard]] auto note_for_degree(int degree) const -> Note;
  [[nodiscard]] auto rising_edge(bool current, bool &previous) -> bool;
};

} // namespace analogno
