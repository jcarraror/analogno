#include "midi_output.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
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

constexpr auto pitch_bend_center = 8192;
constexpr auto pitch_bend_range = 8191;

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

  if (intent.note_on.has_value()) {
    if (active_note_.has_value()) {
      note_off(*active_note_);
    }

    note_on(intent.note_on->midi_note);
    active_note_ = intent.note_on->midi_note;
  }

  pitch_bend(intent.controls.pitch_bend);
  control_change(cc_expression, midi7(intent.controls.expression));
  control_change(cc_filter_cutoff, midi7(intent.controls.filter_cutoff));
  control_change(cc_filter_resonance, midi7(intent.controls.filter_resonance));
  control_change(cc_modulation, midi7(intent.controls.modulation));
  control_change(cc_vibrato, midi7(intent.controls.vibrato));
}

auto MidiOutput::all_notes_off() -> void {
  if (active_note_.has_value()) {
    note_off(*active_note_);
    active_note_.reset();
  }

  control_change(123, 0);
}

auto MidiOutput::note_on(int midi_note) -> void {
  send(midi_, {
                  status(status_note_on, channel),
                  midi_note_byte(midi_note),
                  byte(velocity),
              });
}

auto MidiOutput::note_off(int midi_note) -> void {
  send(midi_, {
                  status(status_note_off, channel),
                  midi_note_byte(midi_note),
                  byte(0),
              });
}

auto MidiOutput::control_change(std::uint8_t controller, std::uint8_t value)
    -> void {
  send(midi_, {
                  status(status_control_change, channel),
                  byte(controller),
                  byte(value),
              });
}

auto MidiOutput::pitch_bend(float normalized) -> void {
  const auto clamped = std::clamp(normalized, -1.0F, 1.0F);
  const auto bend = pitch_bend_center +
                    static_cast<int>(std::lround(clamped * pitch_bend_range));
  const auto safe_bend = std::clamp(bend, 0, 16383);

  const auto lsb = static_cast<std::uint8_t>(safe_bend & 0x7F);
  const auto msb = static_cast<std::uint8_t>((safe_bend >> 7) & 0x7F);

  send(midi_, {
                  status(status_pitch_bend, channel),
                  byte(lsb),
                  byte(msb),
              });
}

auto MidiOutput::midi7(float normalized) -> std::uint8_t {
  const auto clamped = std::clamp(normalized, 0.0F, 1.0F);
  return static_cast<std::uint8_t>(std::lround(clamped * 127.0F));
}

auto MidiOutput::send(RtMidiOut &midi, std::vector<unsigned char> message)
    -> void {
  midi.sendMessage(&message);
}

} // namespace analogno
