#pragma once

#include "music_types.hpp"

#include <RtMidi.h>

#include <array>
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
  static constexpr auto midi_channel_count = std::size_t{16};
  static constexpr auto midi_cc_count = std::size_t{128};
  static constexpr auto midi_note_count = std::size_t{128};

  RtMidiOut midi_;
  std::array<bool, midi_channel_count * midi_note_count> active_notes_{};
  std::array<std::optional<std::uint8_t>, midi_cc_count> last_cc_{};
  std::optional<int> last_pitch_bend_{};

  auto note_on(const Note &note) -> void;
  auto note_off(const Note &note) -> void;
  auto control_change(std::uint8_t controller, std::uint8_t value) -> void;
  auto pitch_bend(float normalized) -> void;

  static auto note_index(const Note &note) -> std::size_t;
  static auto control_value(float normalized) -> std::uint8_t;
  static auto pitch_bend_value(float normalized) -> int;
  static auto valid_midi_note(int note) -> bool;
  static auto send(RtMidiOut &midi, std::vector<unsigned char> message) -> void;
};

} // namespace analogno
