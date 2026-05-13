#pragma once

#include "controller_state.hpp"
#include "music_types.hpp"

#include <array>
#include <optional>

namespace analogno {

class MusicMapper final {
public:
  [[nodiscard]] MusicalIntent map(const ControllerState &controller);
  [[nodiscard]] ContinuousControls
  map_controls(const ControllerState &controller) const;

private:
  static constexpr auto playable_button_count = std::size_t{4};

  int root_midi_note_{48};
  int octave_offset_{};
  ScaleKind scale_{ScaleKind::minor_pentatonic};

  bool previous_l1_{};
  bool previous_r1_{};
  bool previous_dpad_up_{};
  bool previous_dpad_down_{};
  bool previous_dpad_left_{};
  bool previous_dpad_right_{};
  bool previous_guide_{};

  std::array<std::optional<Note>, playable_button_count> active_notes_{};

  void map_note_buttons(const ControllerState &controller,
                        MusicalIntent &intent);
  void update_mode_buttons(const ControllerState &controller,
                           MusicalIntent &intent);

  [[nodiscard]] Note note_for_degree(int degree) const;
  [[nodiscard]] bool rising_edge(bool current, bool &previous);
};

} // namespace analogno
