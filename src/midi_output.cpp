#include "midi_output.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace analogno {
namespace {

constexpr auto status_note_off = std::uint8_t{0x80};
constexpr auto status_note_on = std::uint8_t{0x90};
constexpr auto status_program_change = std::uint8_t{0xC0};
constexpr auto status_control_change = std::uint8_t{0xB0};
constexpr auto status_pitch_bend = std::uint8_t{0xE0};

constexpr auto cc_breath_controller = std::uint8_t{2};
constexpr auto cc_channel_volume = std::uint8_t{7};
constexpr auto cc_pan = std::uint8_t{10};
constexpr auto cc_expression = std::uint8_t{11};
constexpr auto cc_filter_cutoff = std::uint8_t{74};
constexpr auto cc_filter_resonance = std::uint8_t{71};
constexpr auto cc_modulation = std::uint8_t{1};
constexpr auto cc_vibrato = std::uint8_t{76};
constexpr auto cc_bank_select_msb = std::uint8_t{0};
constexpr auto cc_bank_select_lsb = std::uint8_t{32};
constexpr auto cc_all_notes_off = std::uint8_t{123};

constexpr auto pitch_bend_center = 8192;
constexpr auto pitch_bend_range = 8191;
constexpr auto pitch_bend_deadband = 16;
constexpr auto expression_floor = std::uint8_t{72};

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

std::uint8_t expression_value(float normalized) {
  const auto clamped = std::clamp(normalized, 0.0F, 1.0F);
  const auto scaled = static_cast<int>(std::lround(
      static_cast<float>(expression_floor) +
      clamped * static_cast<float>(127U - expression_floor)));
  return static_cast<std::uint8_t>(std::clamp(scaled, 0, 127));
}

void rtmidi_error_callback(RtMidiError::Type type,
                           const std::string &message,
                           void *) {
  static auto last_print = std::chrono::steady_clock::time_point{};
  static int suppressed = 0;

  const auto now = std::chrono::steady_clock::now();

  if (now - last_print < std::chrono::seconds{2}) {
    ++suppressed;
    return;
  }

  if (suppressed > 0) {
    std::cerr << "MIDI output warning: suppressed " << suppressed
              << " repeated RtMidi errors\n";
    suppressed = 0;
  }

  std::cerr << "MIDI output warning"
            << " type=" << static_cast<int>(type)
            << ": " << message << '\n';

  last_print = now;
}

} // namespace

MidiOutput::MidiOutput(std::string port_name)
    : midi_{RtMidi::UNSPECIFIED, "Analogno"} {
  channel_volumes_.fill(100);
  channel_pans_.fill(64);
  midi_.setErrorCallback(&rtmidi_error_callback, nullptr);
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
  control_change(cc_breath_controller, control_value(intent.controls.breath));
  control_change(cc_expression, expression_value(intent.controls.expression));
  control_change(cc_filter_cutoff,
                 control_value(intent.controls.filter_cutoff));
  control_change(cc_filter_resonance,
                 control_value(intent.controls.filter_resonance));
  control_change(cc_modulation, control_value(intent.controls.modulation));
  control_change(cc_vibrato, control_value(intent.controls.vibrato));
}

void MidiOutput::apply_notes_only(const std::vector<Note> &note_offs,
                                  const std::vector<Note> &note_ons) {
  for (const auto &note : note_offs) {
    note_off(note);
  }

  std::uint16_t channels_reset{0};

  for (const auto &note : note_ons) {
    const auto ch = std::clamp(note.channel, 0, 15);
    const auto ch_bit =
        static_cast<std::uint16_t>(std::uint16_t{1}
                                   << static_cast<unsigned>(ch));

    if ((channels_reset & ch_bit) == 0) {
      channels_reset = static_cast<std::uint16_t>(channels_reset | ch_bit);

      const auto vol = static_cast<std::uint8_t>(
          std::clamp(channel_volumes_[static_cast<std::size_t>(ch)], 0, 127));
      send(midi_, {
                      status(status_control_change, ch),
                      byte(cc_channel_volume),
                      byte(vol),
                  });

      const auto pan_val = static_cast<std::uint8_t>(
          std::clamp(channel_pans_[static_cast<std::size_t>(ch)], 0, 127));
      send(midi_, {
                      status(status_control_change, ch),
                      byte(cc_pan),
                      byte(pan_val),
                  });

      send(midi_, {
                      status(status_control_change, ch),
                      byte(cc_expression),
                      byte(std::uint8_t{127}),
                  });
    }

    note_on(note);
  }
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
                    status(status_control_change,
                           static_cast<int>(channel_index)),
                    byte(cc_all_notes_off),
                    byte(0),
                });
  }

  active_notes_.fill(false);
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

  if (send(midi_, {
                  status(status_note_on, note.channel),
                  midi_note_byte(midi_note),
                  byte(static_cast<std::uint8_t>(
                      std::clamp(note.velocity, 1, 127))),
              })) {
    active_notes_[index] = true;
  }
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

  if (send(midi_, {
                  status(status_note_off, note.channel),
                  midi_note_byte(midi_note),
                  byte(0),
              })) {
    active_notes_[index] = false;
  }
}

void MidiOutput::control_change(std::uint8_t controller, std::uint8_t value) {
  const auto index = static_cast<std::size_t>(controller);
  const auto previous = last_cc_[index];

  if (previous.has_value() && *previous == value) {
    return;
  }

  if (send(midi_, {
                  status(status_control_change, live_channel_),
                  byte(controller),
                  byte(value),
              })) {
    last_cc_[index] = value;
  }
}

void MidiOutput::pitch_bend(float normalized) {
  const auto bend = pitch_bend_value(normalized);

  if (last_pitch_bend_.has_value() &&
      std::abs(*last_pitch_bend_ - bend) < pitch_bend_deadband) {
    return;
  }

  const auto lsb = static_cast<std::uint8_t>(bend & 0x7F);
  const auto msb = static_cast<std::uint8_t>((bend >> 7) & 0x7F);

  if (send(midi_, {
                  status(status_pitch_bend, live_channel_),
                  byte(lsb),
                  byte(msb),
              })) {
    last_pitch_bend_ = bend;
  }
}

void MidiOutput::set_live_channel(int ch) {
  ch = std::clamp(ch, 0, 15);

  if (live_channel_ == ch) {
    return;
  }

  live_channel_ = ch;
  last_cc_.fill(std::nullopt);
  last_pitch_bend_.reset();
}

void MidiOutput::set_channel_volume(int ch, int volume) {
  ch = std::clamp(ch, 0, 15);
  volume = std::clamp(volume, 0, 127);
  channel_volumes_[static_cast<std::size_t>(ch)] = volume;
  send(midi_, {
                  status(status_control_change, ch),
                  byte(cc_channel_volume),
                  byte(static_cast<std::uint8_t>(volume)),
              });
}

void MidiOutput::set_channel_pan(int ch, int pan) {
  ch = std::clamp(ch, 0, 15);
  pan = std::clamp(pan, 0, 127);
  channel_pans_[static_cast<std::size_t>(ch)] = pan;
  send(midi_, {
                  status(status_control_change, ch),
                  byte(cc_pan),
                  byte(static_cast<std::uint8_t>(pan)),
              });
}

void MidiOutput::program_change(int program, int bank, int ch) {
  const bool is_percussion = bank == 128;

  if (is_percussion) {
    ch = 9;
  }

  ch = std::clamp(ch, 0, 15);
  program = std::clamp(program, 0, 127);
  bank = std::clamp(bank, 0, 128);

  for (auto n = 0; n < static_cast<int>(midi_note_count); ++n) {
    const auto note = Note{.midi_note = n, .channel = ch};

    if (active_notes_[note_index(note)]) {
      note_off(note);
    }
  }

  send(midi_, {
                  status(status_control_change, ch),
                  byte(cc_all_notes_off),
                  byte(0),
              });

  if (!is_percussion) {
    send(midi_, {
                    status(status_control_change, ch),
                    byte(cc_bank_select_msb),
                    byte(static_cast<std::uint8_t>(bank & 0x7F)),
                });

    send(midi_, {
                    status(status_control_change, ch),
                    byte(cc_bank_select_lsb),
                    byte(0),
                });
  }

  send(midi_, {
                  status(status_control_change, ch),
                  byte(cc_channel_volume),
                  byte(static_cast<std::uint8_t>(
                      std::clamp(channel_volumes_[static_cast<std::size_t>(ch)], 0, 127))),
              });

  send(midi_, {
                  status(status_control_change, ch),
                  byte(cc_pan),
                  byte(static_cast<std::uint8_t>(
                      std::clamp(channel_pans_[static_cast<std::size_t>(ch)], 0, 127))),
              });

  send(midi_, {
                  status(status_control_change, ch),
                  byte(cc_expression),
                  byte(std::uint8_t{127}),
              });

  send(midi_, {
                  status(status_program_change, ch),
                  byte(static_cast<std::uint8_t>(program & 0x7F)),
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

bool MidiOutput::send(RtMidiOut &midi, std::vector<unsigned char> message) {
  static auto last_print = std::chrono::steady_clock::time_point{};
  static int suppressed = 0;

  try {
    midi.sendMessage(&message);
    return true;
  } catch (const RtMidiError &error) {
    const auto now = std::chrono::steady_clock::now();

    if (now - last_print < std::chrono::seconds{2}) {
      ++suppressed;
      return false;
    }

    if (suppressed > 0) {
      std::cerr << "MIDI send failed: suppressed " << suppressed
                << " repeated errors\n";
      suppressed = 0;
    }

    std::cerr << "MIDI send failed: " << error.getMessage() << '\n';
    last_print = now;
    return false;
  } catch (const std::exception &error) {
    const auto now = std::chrono::steady_clock::now();

    if (now - last_print < std::chrono::seconds{2}) {
      ++suppressed;
      return false;
    }

    if (suppressed > 0) {
      std::cerr << "MIDI send failed: suppressed " << suppressed
                << " repeated errors\n";
      suppressed = 0;
    }

    std::cerr << "MIDI send failed: " << error.what() << '\n';
    last_print = now;
    return false;
  } catch (...) {
    const auto now = std::chrono::steady_clock::now();

    if (now - last_print < std::chrono::seconds{2}) {
      ++suppressed;
      return false;
    }

    if (suppressed > 0) {
      std::cerr << "MIDI send failed: suppressed " << suppressed
                << " repeated errors\n";
      suppressed = 0;
    }

    std::cerr << "MIDI send failed: unknown error\n";
    last_print = now;
    return false;
  }
}

} // namespace analogno