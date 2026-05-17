#pragma once

#include "web_state.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace analogno {

class WebSocketServer final {
public:
  struct SampleTrimRequest final {
    float start{};
    float end{1.0F};
  };

  struct PatchRequest final {
    int bank{};
    std::uint8_t program{};
  };

  struct WavetableRequest final {
    std::vector<float> samples{};
    std::vector<float> morph_samples{};
  };

  struct WavetableControls final {
    float morph{};
    float noise{};
    float unison{};
  };

  struct SeqStepConfig final {
    bool active{};
    bool tie{};
    int degree{};
    int velocity{100};
    int midi_note{-1};
  };

  struct SeqTrackConfig final {
    int midi_channel{-1}; // -1 = keep existing
    int midi_program{0};
    int midi_bank{0};
    int sample_bank{-1}; // -1 = MIDI; 0..7 = sampler bank
    int loop_length{32};
    bool muted{false};
    std::vector<SeqStepConfig> steps{};
  };

  struct SeqConfig final {
    static constexpr int max_step_count = 64;
    static constexpr int max_tracks = 16; // MIDI channel limit
    float bpm{120.0F};
    int gate_pct{50};
    int step_count{32};
    int step_division{16};
    std::vector<SeqTrackConfig> tracks{};
  };

  struct VoiceSeqConfig final {
    bool enabled{};
    bool recording{};
    std::string mode{"percussion"};
    bool snap_to_scale{true};
    float sensitivity{0.65F};
    float timing_offset_ms{};
  };

  struct Panic final {};
  struct SetCaptureDevice final {
    std::optional<int> device_index{};
  };
  struct SetSampleTrim final {
    SampleTrimRequest trim{};
  };
  struct SetActiveBank final {
    std::size_t bank{};
  };
  struct SaveSample final {
    std::size_t bank{};
  };
  struct SetPatch final {
    PatchRequest patch{};
  };
  struct SetWavetable final {
    WavetableRequest wavetable{};
  };
  struct SetWavetableControls final {
    WavetableControls controls{};
  };
  struct SeqPlay final {};
  struct SeqStop final {};
  struct SeqAddTrack final {};
  struct SeqRemoveTrack final {
    int track{};
  };
  struct SeqSelectStep final {
    int step{};
  };
  struct SeqSelectTrack final {
    int track{};
  };
  struct SetSeq final {
    SeqConfig config{};
  };
  struct SetSoundfont final {
    std::string path{};
  };
  struct SetBlowMode final {
    bool enabled{};
  };
  struct SetBlowSensitivity final {
    float sensitivity{};
  };
  struct SetVoiceSeq final {
    VoiceSeqConfig config{};
  };

  using Command = std::variant<Panic, SetCaptureDevice, SetSampleTrim,
                               SetActiveBank, SaveSample, SetPatch,
                               SetWavetable, SetWavetableControls, SeqPlay,
                               SeqStop, SeqAddTrack, SeqRemoveTrack,
                               SeqSelectStep, SeqSelectTrack, SetSeq,
                               SetSoundfont, SetBlowMode, SetBlowSensitivity,
                               SetVoiceSeq>;

  explicit WebSocketServer(std::uint16_t port = 8765);
  ~WebSocketServer();

  WebSocketServer(const WebSocketServer &) = delete;
  WebSocketServer &operator=(const WebSocketServer &) = delete;

  WebSocketServer(WebSocketServer &&) = delete;
  WebSocketServer &operator=(WebSocketServer &&) = delete;

  void start();
  void stop();
  void publish_runtime(const WebRuntimeState &state);
  void publish_library(const WebLibraryState &state);

  [[nodiscard]] std::vector<Command> consume_commands();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace analogno
