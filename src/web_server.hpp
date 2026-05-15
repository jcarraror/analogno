#pragma once

#include "web_state.hpp"

#include <cstdint>
#include <memory>
#include <optional>

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

  struct SeqStepConfig final {
    bool active{};
    int degree{};
    int velocity{100};
    int midi_note{-1};
  };

  struct SeqTrackConfig final {
    int midi_channel{-1}; // -1 = keep existing
    int midi_program{0};
    int midi_bank{0};
    bool muted{false};
    std::array<SeqStepConfig, 16> steps{};
  };

  struct SeqConfig final {
    static constexpr int step_count = 16;
    static constexpr int max_tracks = 16; // MIDI channel limit
    float bpm{120.0F};
    int gate_pct{50};
    std::vector<SeqTrackConfig> tracks{};
  };

  explicit WebSocketServer(std::uint16_t port = 8765);
  ~WebSocketServer();

  WebSocketServer(const WebSocketServer &) = delete;
  WebSocketServer &operator=(const WebSocketServer &) = delete;

  WebSocketServer(WebSocketServer &&) = delete;
  WebSocketServer &operator=(WebSocketServer &&) = delete;

  void start();
  void stop();
  void publish(const WebRuntimeState &state);

  [[nodiscard]] bool consume_panic_requested();
  [[nodiscard]] std::optional<int> consume_capture_device_request();
  [[nodiscard]] std::optional<SampleTrimRequest> consume_sample_trim_request();
  [[nodiscard]] std::optional<std::size_t> consume_active_bank_request();
  [[nodiscard]] std::optional<std::size_t> consume_save_sample_request();
  [[nodiscard]] std::optional<PatchRequest> consume_patch_request();
  [[nodiscard]] std::optional<std::vector<float>> consume_wavetable_request();
  [[nodiscard]] bool consume_seq_play();
  [[nodiscard]] bool consume_seq_stop();
  [[nodiscard]] bool consume_seq_add_track();
  [[nodiscard]] std::optional<int> consume_seq_remove_track();
  [[nodiscard]] std::optional<int> consume_seq_select_step();
  [[nodiscard]] std::optional<int> consume_seq_select_track();
  [[nodiscard]] std::optional<SeqConfig> consume_seq_config();
  [[nodiscard]] std::optional<std::string> consume_soundfont_request();
  [[nodiscard]] std::optional<bool> consume_blow_mode_request();
  [[nodiscard]] std::optional<float> consume_blow_sensitivity_request();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace analogno
