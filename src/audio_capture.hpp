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
  AudioCapture &operator=(const AudioCapture &) = delete;

  AudioCapture(AudioCapture &&) = delete;
  AudioCapture &operator=(AudioCapture &&) = delete;

  [[nodiscard]] const std::vector<CaptureDeviceInfo> &devices() const;

  void start(std::optional<std::uint32_t> preferred_index = std::nullopt);
  void stop();
  void begin_sample_recording();
  void end_sample_recording();

  [[nodiscard]] bool is_running() const;
  [[nodiscard]] bool is_sample_recording() const;
  [[nodiscard]] float level() const;
  [[nodiscard]] AudioFeatures consume_features();
  [[nodiscard]] std::vector<float> waveform() const;
  [[nodiscard]] std::vector<float> spec_samples() const;
  [[nodiscard]] std::vector<float> consume_analysis_frames(std::size_t max_frames);
  void start_loopback();
  [[nodiscard]] ma_context *shared_context();
  [[nodiscard]] std::optional<std::vector<float>> consume_captured_sample();
  [[nodiscard]] std::size_t captured_sample_frames() const;
  [[nodiscard]] const std::string &selected_device_name() const;
  [[nodiscard]] std::optional<std::uint32_t> selected_device_index() const;

private:
  static constexpr auto waveform_sample_count = std::size_t{128};
  static constexpr auto spec_sample_count     = std::size_t{2048};
  static constexpr auto max_sample_frames = std::size_t{480000};
  static constexpr auto analysis_sample_count = std::size_t{48000 * 2};

  ma_context context_{};
  ma_device device_{};
  ma_context loopback_context_{};
  ma_device loopback_device_{};
  std::atomic<bool> loopback_ready_{false};

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
  std::array<std::atomic<float>, spec_sample_count>     spec_{};
  std::array<std::atomic<float>, analysis_sample_count> analysis_{};
  std::array<std::atomic<float>, max_sample_frames> recording_sample_{};
  std::atomic<std::size_t> waveform_write_index_{};
  std::atomic<std::size_t> spec_write_index_{};
  std::atomic<std::size_t> analysis_write_index_{};
  std::atomic<std::size_t> analysis_read_index_{};
  std::atomic<std::size_t> recording_write_index_{};
  std::vector<float> captured_sample_{};
  std::mutex sample_mutex_{};
  bool context_ready_{};
  bool device_ready_{};
  bool running_{};

  void enumerate_devices();
  [[nodiscard]] std::optional<ma_device_id>
  choose_device(std::optional<std::uint32_t> preferred_index) const;
  [[nodiscard]] std::optional<std::uint32_t> default_device_index() const;

  static void capture_callback(ma_device *device, void *output,
                               const void *input, ma_uint32 frame_count);
  static void loopback_callback(ma_device *device, void *output,
                                const void *input, ma_uint32 frame_count);
};

} // namespace analogno
