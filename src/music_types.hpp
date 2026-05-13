#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace analogno {

enum class ScaleKind : std::uint8_t {
  minor_pentatonic,
  major,
  natural_minor,
  dorian,
  phrygian,
  chromatic,
};

struct Scale final {
  ScaleKind kind{};
  std::string_view name{};
  std::array<int, 12> semitones{};
  int size{};
};

struct Note final {
  int midi_note{};
  int degree{};
  int octave{};
  int velocity{100};
  int channel{};
};

struct ContinuousControls final {
  float pitch_bend{};       // -1..1
  float expression{};       // 0..1
  float filter_cutoff{};    // 0..1
  float filter_resonance{}; // 0..1
  float modulation{};       // 0..1
  float vibrato{};          // 0..1
};

struct MusicalIntent final {
  std::vector<Note> note_ons{};
  std::vector<Note> note_offs{};
  bool note_off_all{};

  int root_midi_note{60};
  int octave_offset{};
  ScaleKind scale{ScaleKind::minor_pentatonic};

  ContinuousControls controls{};
};

struct AudioFeatures final {
  float level{};
  float envelope{};
  bool gate_open{};
  bool onset{};
  int velocity{};
};

Scale scale_for(ScaleKind kind);
std::string_view scale_name(ScaleKind kind);

} // namespace analogno
