#pragma once

#include "controller_state.hpp"
#include "music_types.hpp"

#include <array>
#include <optional>

namespace analogno {

class MusicMapper final {
public:
  [[nodiscard]] auto map(const ControllerState &controller) -> MusicalIntent;

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

  auto map_note_buttons(const ControllerState &controller,
                        MusicalIntent &intent) -> void;
  auto update_mode_buttons(const ControllerState &controller,
                           MusicalIntent &intent) -> void;

  [[nodiscard]] auto note_for_degree(int degree) const -> Note;
  [[nodiscard]] auto rising_edge(bool current, bool &previous) -> bool;
};

} // namespace analogno
