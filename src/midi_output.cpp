#include "midi_output.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

namespace analogno {
namespace {

constexpr auto status_note_off = std::uint8_t{0x80};
constexpr auto status_note_on = std::uint8_t{0x90};
constexpr auto status_control_change = std::uint8_t{0xB0};
constexpr auto status_pitch_bend = std::uint8_t{0xE0};

constexpr auto cc_expression = std::uint8_t{11};
constexpr auto cc_filter_cutoff = std::uint8_t{74};
constexpr auto cc_filter_resonance = std::uint8_t{71};
constexpr auto cc_modulation = std::uint8_t{1};
constexpr auto cc_vibrato = std::uint8_t{76};
constexpr auto cc_all_notes_off = std::uint8_t{123};

constexpr auto pitch_bend_center = 8192;
constexpr auto pitch_bend_range = 8191;
constexpr auto pitch_bend_deadband = 16;

auto status(std::uint8_t base, std::uint8_t channel) -> unsigned char {
  return static_cast<unsigned char>(base | (channel & 0x0F));
}

auto byte(std::uint8_t value) -> unsigned char {
  return static_cast<unsigned char>(value & 0x7F);
}

auto midi_note_byte(int note) -> unsigned char {
  return byte(static_cast<std::uint8_t>(std::clamp(note, 0, 127)));
}

} // namespace

MidiOutput::MidiOutput(std::string port_name)
    : midi_{RtMidi::UNSPECIFIED, "Analogno"} {
  midi_.openVirtualPort(port_name);
  std::cout << "opened virtual MIDI output: " << port_name << '\n';
}

MidiOutput::~MidiOutput() {
  try {
    all_notes_off();
  } catch (...) {
  }
}

auto MidiOutput::apply(const MusicalIntent &intent) -> void {
  if (intent.note_off_all) {
    all_notes_off();
  }

  for (const auto &note : intent.note_offs) {
    note_off(note.midi_note);
  }

  for (const auto &note : intent.note_ons) {
    note_on(note.midi_note);
  }

  pitch_bend(intent.controls.pitch_bend);
  control_change(cc_expression, control_value(intent.controls.expression));
  control_change(cc_filter_cutoff, control_value(intent.controls.filter_cutoff));
  control_change(cc_filter_resonance,
                 control_value(intent.controls.filter_resonance));
  control_change(cc_modulation, control_value(intent.controls.modulation));
  control_change(cc_vibrato, control_value(intent.controls.vibrato));
}

auto MidiOutput::all_notes_off() -> void {
  for (auto note = 0; note < static_cast<int>(active_notes_.size()); ++note) {
    if (active_notes_[static_cast<std::size_t>(note)]) {
      note_off(note);
    }
  }

  control_change(cc_all_notes_off, 0);
}

auto MidiOutput::note_on(int midi_note) -> void {
  if (!valid_midi_note(midi_note)) {
    return;
  }

  const auto index = static_cast<std::size_t>(midi_note);

  if (active_notes_[index]) {
    return;
  }

  active_notes_[index] = true;

  send(midi_, {
                  status(status_note_on, channel),
                  midi_note_byte(midi_note),
                  byte(velocity),
              });
}

auto MidiOutput::note_off(int midi_note) -> void {
  if (!valid_midi_note(midi_note)) {
    return;
  }

  const auto index = static_cast<std::size_t>(midi_note);

  if (!active_notes_[index]) {
    return;
  }

  active_notes_[index] = false;

  send(midi_, {
                  status(status_note_off, channel),
                  midi_note_byte(midi_note),
                  byte(0),
              });
}

auto MidiOutput::control_change(std::uint8_t controller, std::uint8_t value)
    -> void {
  const auto index = static_cast<std::size_t>(controller);
  const auto previous = last_cc_[index];

  if (previous.has_value() && *previous == value) {
    return;
  }

  last_cc_[index] = value;

  send(midi_, {
                  status(status_control_change, channel),
                  byte(controller),
                  byte(value),
              });
}

auto MidiOutput::pitch_bend(float normalized) -> void {
  const auto bend = pitch_bend_value(normalized);

  if (last_pitch_bend_.has_value() &&
      std::abs(*last_pitch_bend_ - bend) < pitch_bend_deadband) {
    return;
  }

  last_pitch_bend_ = bend;

  const auto lsb = static_cast<std::uint8_t>(bend & 0x7F);
  const auto msb = static_cast<std::uint8_t>((bend >> 7) & 0x7F);

  send(midi_, {
                  status(status_pitch_bend, channel),
                  byte(lsb),
                  byte(msb),
              });
}

auto MidiOutput::control_value(float normalized) -> std::uint8_t {
  const auto clamped = std::clamp(normalized, 0.0F, 1.0F);
  return static_cast<std::uint8_t>(std::lround(clamped * 127.0F));
}

auto MidiOutput::pitch_bend_value(float normalized) -> int {
  const auto clamped = std::clamp(normalized, -1.0F, 1.0F);
  const auto bend = pitch_bend_center +
                    static_cast<int>(std::lround(clamped * pitch_bend_range));
  return std::clamp(bend, 0, 16383);
}

auto MidiOutput::valid_midi_note(int note) -> bool {
  return note >= 0 && note < static_cast<int>(midi_note_count);
}

auto MidiOutput::send(RtMidiOut &midi, std::vector<unsigned char> message)
    -> void {
  midi.sendMessage(&message);
}

} // namespace analogno
