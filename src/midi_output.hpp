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
  MidiOutput &operator=(const MidiOutput &) = delete;

  MidiOutput(MidiOutput &&) = delete;
  MidiOutput &operator=(MidiOutput &&) = delete;

  ~MidiOutput();

  void apply(const MusicalIntent &intent);
  void all_notes_off();

private:
  static constexpr auto channel = std::uint8_t{0};
  static constexpr auto midi_channel_count = std::size_t{16};
  static constexpr auto midi_cc_count = std::size_t{128};
  static constexpr auto midi_note_count = std::size_t{128};

  RtMidiOut midi_;
  std::array<bool, midi_channel_count * midi_note_count> active_notes_{};
  std::array<std::optional<std::uint8_t>, midi_cc_count> last_cc_{};
  std::optional<int> last_pitch_bend_{};

  void note_on(const Note &note);
  void note_off(const Note &note);
  void control_change(std::uint8_t controller, std::uint8_t value);
  void pitch_bend(float normalized);

  static std::size_t note_index(const Note &note);
  static std::uint8_t control_value(float normalized);
  static int pitch_bend_value(float normalized);
  static bool valid_midi_note(int note);
  static void send(RtMidiOut &midi, std::vector<unsigned char> message);
};

} // namespace analogno
