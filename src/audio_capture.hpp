#pragma once

#include "music_types.hpp"

#include <miniaudio.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace analogno {

struct CaptureDeviceInfo final {
  std::uint32_t index{};
  std::string name{};
  bool is_default{};
};

class AudioCapture final {
public:
  AudioCapture();
  ~AudioCapture();

  AudioCapture(const AudioCapture &) = delete;
  auto operator=(const AudioCapture &) -> AudioCapture & = delete;

  AudioCapture(AudioCapture &&) = delete;
  auto operator=(AudioCapture &&) -> AudioCapture & = delete;

  [[nodiscard]] auto devices() const -> const std::vector<CaptureDeviceInfo> &;

  auto start(std::optional<std::uint32_t> preferred_index = std::nullopt)
      -> void;
  auto stop() -> void;
  auto begin_sample_recording() -> void;
  auto end_sample_recording() -> void;

  [[nodiscard]] auto is_running() const -> bool;
  [[nodiscard]] auto is_sample_recording() const -> bool;
  [[nodiscard]] auto level() const -> float;
  [[nodiscard]] auto consume_features() -> AudioFeatures;
  [[nodiscard]] auto waveform() const -> std::vector<float>;
  [[nodiscard]] auto consume_captured_sample() -> std::optional<std::vector<float>>;
  [[nodiscard]] auto captured_sample_frames() const -> std::size_t;
  [[nodiscard]] auto selected_device_name() const -> const std::string &;
  [[nodiscard]] auto selected_device_index() const
      -> std::optional<std::uint32_t>;

private:
  static constexpr auto waveform_sample_count = std::size_t{128};
  static constexpr auto max_sample_frames = std::size_t{480000};

  ma_context context_{};
  ma_device device_{};

  std::vector<CaptureDeviceInfo> devices_{};
  std::string selected_device_name_{"none"};
  std::optional<std::uint32_t> selected_device_index_{};

  std::atomic<float> level_{0.0F};
  std::atomic<float> envelope_{0.0F};
  std::atomic<bool> gate_open_{false};
  std::atomic<std::uint32_t> onset_count_{};
  std::atomic<int> onset_velocity_{};
  std::atomic<std::size_t> captured_sample_frames_{};
  std::atomic<bool> sample_recording_{false};
  std::uint32_t onset_cooldown_frames_{};
  std::array<std::atomic<float>, waveform_sample_count> waveform_{};
  std::atomic<std::size_t> waveform_write_index_{};
  std::vector<float> recording_sample_{};
  std::vector<float> captured_sample_{};
  std::mutex sample_mutex_{};
  bool context_ready_{};
  bool device_ready_{};
  bool running_{};

  auto enumerate_devices() -> void;
  [[nodiscard]] auto
  choose_device(std::optional<std::uint32_t> preferred_index) const
      -> std::optional<ma_device_id>;
  [[nodiscard]] auto default_device_index() const
      -> std::optional<std::uint32_t>;

  static auto capture_callback(ma_device *device, void *output,
                               const void *input, ma_uint32 frame_count)
      -> void;
};

} // namespace analogno
