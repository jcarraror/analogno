#pragma once

#include "audio_capture.hpp"
#include "audio_sampler.hpp"
#include "controller_state.hpp"
#include "music_types.hpp"
#include "sequencer.hpp"
#include "voice_sequencer.hpp"
#include "web_state.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace analogno {

struct WaveformSketch final {
  bool active{false};
  std::vector<std::pair<float, float>> points{};
  std::vector<float> committed{};
  std::vector<std::pair<float, float>> committed_points{};
};

[[nodiscard]] std::vector<float>
build_wavetable(const std::vector<std::pair<float, float>> &points,
                std::size_t n);

[[nodiscard]] WebRuntimeState make_web_state(
    const ControllerState &controller, const MusicalIntent &intent,
    const std::vector<int> &active_notes, const AudioCapture &audio_capture,
    const AudioSampler &audio_sampler, const AudioFeatures &audio_features,
    int midi_program, int midi_bank, const WaveformSketch &sketch,
    const Sequencer &seq, bool piano_roll_visible, bool spectrogram_visible,
    bool blow_mode, float wavetable_morph, float wavetable_noise,
    float wavetable_unison, float blow_sensitivity, bool blow_active,
    float blow_level, const VoiceSequencerStatus &voice_seq_status,
    const std::vector<float> &spec_samples, float signals_volume);

[[nodiscard]] WebLibraryState make_web_library_state(
    const std::vector<WebPreset> &presets,
    const std::vector<std::string> &soundfonts,
    const std::string &active_soundfont);

} // namespace analogno
