#pragma once

#include "active_note_tracker.hpp"
#include "audio_capture.hpp"
#include "audio_sampler.hpp"
#include "controller_state.hpp"
#include "drum_pad.hpp"
#include "midi_output.hpp"
#include "music_mapper.hpp"
#include "music_types.hpp"
#include "runtime_state_builder.hpp"
#include "sequencer.hpp"
#include "stem_pipeline.hpp"
#include "transcribe_manager.hpp"
#include "voice_sequencer.hpp"
#include "web_server.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace analogno {

struct RibbonFinger {
    bool active{false};
    int midi_channel{};
    int midi_note{-1};
    int active_channel{-1};
};

struct TouchSwipe {
    float prev_x{};
    float prev_y{};
    bool active{false};
};

struct LedFlash {
    std::array<std::uint8_t, 3> color{};
    std::chrono::steady_clock::time_point show_until{};
};

// Helpers shared by the command handler and the touchpad router in main.

inline void release_ribbon_finger(RibbonFinger& rf,
                                   MidiOutput& midi,
                                   ActiveNoteTracker& active_notes) {
    if (rf.active && rf.midi_note >= 0 && rf.active_channel >= 0) {
        midi.apply_notes_only(
            {{Note{.midi_note = rf.midi_note, .channel = rf.active_channel}}}, {});
        active_notes.apply(
            MusicalIntent{.note_offs = {{Note{.midi_note = rf.midi_note}}}});
    }
    rf.active = false;
    rf.midi_note = -1;
    rf.active_channel = -1;
}

[[nodiscard]] inline int ribbon_channel_for_bank(const RibbonFinger& rf,
                                                  int bank) noexcept {
    return bank == 128 ? 9 : rf.midi_channel;
}

struct AppContext {
    ControllerState& controller;
    MusicMapper& mapper;
    MidiOutput& midi;
    ActiveNoteTracker& active_notes;
    AudioCapture& audio_capture;
    AudioSampler& audio_sampler;
    WebSocketServer& web;
    VoiceSequencer& voice_seq;
    DrumPad& drum_pad;
    Sequencer& seq;
    WaveformSketch& sketch;
    TranscribeManager& transcribe;
    StemPipeline& stem_pipeline;

    bool& sampler_mode;
    int& current_midi_bank;
    int& current_midi_program;
    float& signals_volume;
    float& wavetable_morph;
    float& wavetable_noise;
    float& wavetable_unison;
    std::string& stems_folder;
    const std::string& stems_base_dir;
    MusicalIntent& last_intent;
    AudioFeatures& last_audio_features;
    const std::string& session_path;

    std::vector<WebPreset>& sf2_presets;
    std::string& active_soundfont;
    const std::vector<std::string>& available_soundfonts;

    std::array<RibbonFinger, 2>& ribbon;
    TouchSwipe& tp_swipe;
    std::deque<LedFlash>& led_queue;
};

} // namespace analogno
