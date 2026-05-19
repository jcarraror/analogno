#pragma once

#include "music_types.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace analogno {

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

} // namespace analogno
