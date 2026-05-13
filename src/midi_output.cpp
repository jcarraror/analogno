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

std::uint8_t channel_byte(int channel) {
  return static_cast<std::uint8_t>(std::clamp(channel, 0, 15));
}

unsigned char status(std::uint8_t base, int channel) {
  return static_cast<unsigned char>(base | channel_byte(channel));
}

unsigned char byte(std::uint8_t value) {
  return static_cast<unsigned char>(value & 0x7F);
}

unsigned char midi_note_byte(int note) {
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

void MidiOutput::apply(const MusicalIntent &intent) {
  if (intent.note_off_all) {
    all_notes_off();
  }

  for (const auto &note : intent.note_offs) {
    note_off(note);
  }

  for (const auto &note : intent.note_ons) {
    note_on(note);
  }

  pitch_bend(intent.controls.pitch_bend);
  control_change(cc_expression, control_value(intent.controls.expression));
  control_change(cc_filter_cutoff,
                 control_value(intent.controls.filter_cutoff));
  control_change(cc_filter_resonance,
                 control_value(intent.controls.filter_resonance));
  control_change(cc_modulation, control_value(intent.controls.modulation));
  control_change(cc_vibrato, control_value(intent.controls.vibrato));
}

void MidiOutput::all_notes_off() {
  for (auto channel_index = std::size_t{0}; channel_index < midi_channel_count;
       ++channel_index) {
    for (auto note = 0; note < static_cast<int>(midi_note_count); ++note) {
      const auto note_value = Note{
          .midi_note = note,
          .degree = 0,
          .octave = 0,
          .velocity = 0,
          .channel = static_cast<int>(channel_index),
      };

      if (active_notes_[note_index(note_value)]) {
        note_off(note_value);
      }
    }
  }

  for (auto channel_index = std::size_t{0}; channel_index < midi_channel_count;
       ++channel_index) {
    send(midi_, {
                    status(status_control_change, static_cast<int>(channel_index)),
                    byte(cc_all_notes_off),
                    byte(0),
                });
  }
}

void MidiOutput::note_on(const Note &note) {
  const auto midi_note = note.midi_note;

  if (!valid_midi_note(midi_note)) {
    return;
  }

  const auto index = note_index(note);

  if (active_notes_[index]) {
    return;
  }

  active_notes_[index] = true;

  send(midi_, {
                  status(status_note_on, note.channel),
                  midi_note_byte(midi_note),
                  byte(static_cast<std::uint8_t>(
                      std::clamp(note.velocity, 1, 127))),
              });
}

void MidiOutput::note_off(const Note &note) {
  const auto midi_note = note.midi_note;

  if (!valid_midi_note(midi_note)) {
    return;
  }

  const auto index = note_index(note);

  if (!active_notes_[index]) {
    return;
  }

  active_notes_[index] = false;

  send(midi_, {
                  status(status_note_off, note.channel),
                  midi_note_byte(midi_note),
                  byte(0),
              });
}

void MidiOutput::control_change(std::uint8_t controller, std::uint8_t value) {
  const auto index = static_cast<std::size_t>(controller);
  const auto previous = last_cc_[index];

  if (previous.has_value() && *previous == value) {
    return;
  }

  last_cc_[index] = value;

  send(midi_, {
                  status(status_control_change, static_cast<int>(channel)),
                  byte(controller),
                  byte(value),
              });
}

void MidiOutput::pitch_bend(float normalized) {
  const auto bend = pitch_bend_value(normalized);

  if (last_pitch_bend_.has_value() &&
      std::abs(*last_pitch_bend_ - bend) < pitch_bend_deadband) {
    return;
  }

  last_pitch_bend_ = bend;

  const auto lsb = static_cast<std::uint8_t>(bend & 0x7F);
  const auto msb = static_cast<std::uint8_t>((bend >> 7) & 0x7F);

  send(midi_, {
                  status(status_pitch_bend, static_cast<int>(channel)),
                  byte(lsb),
                  byte(msb),
              });
}

std::size_t MidiOutput::note_index(const Note &note) {
  return static_cast<std::size_t>(channel_byte(note.channel)) * midi_note_count +
         static_cast<std::size_t>(std::clamp(note.midi_note, 0, 127));
}

std::uint8_t MidiOutput::control_value(float normalized) {
  const auto clamped = std::clamp(normalized, 0.0F, 1.0F);
  return static_cast<std::uint8_t>(std::lround(clamped * 127.0F));
}

int MidiOutput::pitch_bend_value(float normalized) {
  const auto clamped = std::clamp(normalized, -1.0F, 1.0F);
  const auto bend = pitch_bend_center +
                    static_cast<int>(std::lround(clamped * pitch_bend_range));
  return std::clamp(bend, 0, 16383);
}

bool MidiOutput::valid_midi_note(int note) {
  return note >= 0 && note < static_cast<int>(midi_note_count);
}

void MidiOutput::send(RtMidiOut &midi, std::vector<unsigned char> message) {
  midi.sendMessage(&message);
}

} // namespace analogno
