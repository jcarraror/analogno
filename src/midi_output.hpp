#pragma once

#include "music_types.hpp"

#include <RtMidi.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace analogno {

class MidiOutput final {
public:
  explicit MidiOutput(std::string port_name = "Analogno MIDI Out");

  MidiOutput(const MidiOutput &) = delete;
  auto operator=(const MidiOutput &) -> MidiOutput & = delete;

  MidiOutput(MidiOutput &&) = delete;
  auto operator=(MidiOutput &&) -> MidiOutput & = delete;

  ~MidiOutput();

  auto apply(const MusicalIntent &intent) -> void;
  auto all_notes_off() -> void;

private:
  static constexpr auto channel = std::uint8_t{0};
  static constexpr auto velocity = std::uint8_t{100};

  RtMidiOut midi_;
  std::optional<int> active_note_{};

  auto note_on(int midi_note) -> void;
  auto note_off(int midi_note) -> void;
  auto control_change(std::uint8_t controller, std::uint8_t value) -> void;
  auto pitch_bend(float normalized) -> void;

  static auto midi7(float normalized) -> std::uint8_t;
  static auto send(RtMidiOut &midi, std::vector<unsigned char> message) -> void;
};

} // namespace analogno
