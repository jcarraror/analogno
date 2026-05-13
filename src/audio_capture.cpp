#include "audio_capture.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace analogno {
namespace {

constexpr auto sample_rate = ma_uint32{48000};
constexpr auto channels = ma_uint32{1};

} // namespace

AudioCapture::AudioCapture() {
  if (ma_context_init(nullptr, 0, nullptr, &context_) != MA_SUCCESS) {
    std::cerr << "warning: failed to initialize miniaudio context\n";
    return;
  }

  context_ready_ = true;
  enumerate_devices();
}

AudioCapture::~AudioCapture() {
  stop();

  if (context_ready_) {
    ma_context_uninit(&context_);
  }
}

auto AudioCapture::devices() const -> const std::vector<CaptureDeviceInfo> & {
  return devices_;
}

auto AudioCapture::start(std::optional<std::uint32_t> preferred_index) -> void {
  if (!context_ready_) {
    return;
  }

  if (running_) {
    return;
  }

  const auto selected_id = choose_device(preferred_index);
  const auto selected_index =
      selected_id.has_value() ? preferred_index : default_device_index();

  ma_device_config config = ma_device_config_init(ma_device_type_capture);
  config.capture.format = ma_format_f32;
  config.capture.channels = channels;
  config.sampleRate = sample_rate;
  config.dataCallback = &AudioCapture::capture_callback;
  config.pUserData = this;

  if (selected_id.has_value()) {
    config.capture.pDeviceID = &*selected_id;
  }

  if (ma_device_init(&context_, &config, &device_) != MA_SUCCESS) {
    std::cerr << "warning: failed to initialize capture device\n";
    return;
  }

  device_ready_ = true;

  const char *name = device_.capture.name;
  selected_device_name_ = name != nullptr ? name : "unknown";
  selected_device_index_ = selected_index;

  if (ma_device_start(&device_) != MA_SUCCESS) {
    std::cerr << "warning: failed to start capture device\n";
    ma_device_uninit(&device_);
    device_ready_ = false;
    selected_device_name_ = "none";
    selected_device_index_.reset();
    return;
  }

  running_ = true;
  std::cout << "capture device started: " << selected_device_name_ << '\n';
}

auto AudioCapture::stop() -> void {
  if (!device_ready_) {
    return;
  }

  ma_device_uninit(&device_);
  device_ready_ = false;
  running_ = false;
  selected_device_name_ = "none";
  selected_device_index_.reset();
  level_.store(0.0F);

  for (auto &sample : waveform_) {
    sample.store(0.0F);
  }

  waveform_write_index_.store(0);
}

auto AudioCapture::is_running() const -> bool { return running_; }

auto AudioCapture::level() const -> float { return level_.load(); }

auto AudioCapture::waveform() const -> std::vector<float> {
  std::vector<float> samples{};
  samples.reserve(waveform_sample_count);

  const auto write_index = waveform_write_index_.load(std::memory_order_acquire);
  const auto available = std::min(write_index, waveform_sample_count);
  const auto first = write_index >= waveform_sample_count
                         ? write_index - waveform_sample_count
                         : std::size_t{0};

  for (std::size_t i = 0; i < waveform_sample_count - available; ++i) {
    samples.push_back(0.0F);
  }

  for (std::size_t i = 0; i < available; ++i) {
    const auto index = (first + i) % waveform_sample_count;
    samples.push_back(waveform_[index].load(std::memory_order_relaxed));
  }

  return samples;
}

auto AudioCapture::selected_device_name() const -> const std::string & {
  return selected_device_name_;
}

auto AudioCapture::selected_device_index() const
    -> std::optional<std::uint32_t> {
  return selected_device_index_;
}

auto AudioCapture::enumerate_devices() -> void {
  ma_device_info *playback_infos = nullptr;
  ma_uint32 playback_count = 0;
  ma_device_info *capture_infos = nullptr;
  ma_uint32 capture_count = 0;

  if (ma_context_get_devices(&context_, &playback_infos, &playback_count,
                             &capture_infos, &capture_count) != MA_SUCCESS) {
    std::cerr << "warning: failed to enumerate audio devices\n";
    return;
  }

  devices_.clear();

  std::cout << "audio capture devices:\n";

  for (ma_uint32 i = 0; i < capture_count; ++i) {
    const auto name = std::string{capture_infos[i].name};
    const auto is_default = capture_infos[i].isDefault != 0;

    devices_.push_back(CaptureDeviceInfo{
        .index = i,
        .name = name,
        .is_default = is_default,
    });

    std::cout << "  [" << i << "] " << name;

    if (is_default) {
      std::cout << " (default)";
    }

    std::cout << '\n';
  }

  if (devices_.empty()) {
    std::cout << "  none\n";
  }
}

auto AudioCapture::choose_device(std::optional<std::uint32_t> preferred_index)
    const -> std::optional<ma_device_id> {
  if (devices_.empty()) {
    return std::nullopt;
  }

  ma_device_info *playback_infos = nullptr;
  ma_uint32 playback_count = 0;
  ma_device_info *capture_infos = nullptr;
  ma_uint32 capture_count = 0;

  if (ma_context_get_devices(const_cast<ma_context *>(&context_),
                             &playback_infos, &playback_count, &capture_infos,
                             &capture_count) != MA_SUCCESS) {
    return std::nullopt;
  }

  if (preferred_index.has_value() && *preferred_index < capture_count) {
    return capture_infos[*preferred_index].id;
  }

  if (preferred_index.has_value()) {
    std::cerr << "warning: invalid capture device index "
              << *preferred_index << "; using default capture device\n";
  }

  return std::nullopt;
}

auto AudioCapture::default_device_index() const
    -> std::optional<std::uint32_t> {
  for (const auto &device : devices_) {
    if (device.is_default) {
      return device.index;
    }
  }

  return std::nullopt;
}

auto AudioCapture::capture_callback(ma_device *device, void *output,
                                    const void *input, ma_uint32 frame_count)
    -> void {
  static_cast<void>(output);

  auto *self = static_cast<AudioCapture *>(device->pUserData);

  if (self == nullptr || input == nullptr || frame_count == 0) {
    return;
  }

  const auto *samples = static_cast<const float *>(input);
  const auto sample_count = static_cast<std::size_t>(frame_count) * channels;

  auto sum = 0.0F;

  for (std::size_t i = 0; i < sample_count; ++i) {
    sum += samples[i] * samples[i];
  }

  const auto rms = std::sqrt(sum / static_cast<float>(sample_count));
  const auto current = self->level_.load();
  const auto smoothed = current * 0.85F + rms * 0.15F;

  self->level_.store(std::clamp(smoothed * 8.0F, 0.0F, 1.0F));

  auto write_index =
      self->waveform_write_index_.load(std::memory_order_relaxed);

  for (std::size_t i = 0; i < sample_count; ++i) {
    const auto index = write_index % waveform_sample_count;
    self->waveform_[index].store(std::clamp(samples[i], -1.0F, 1.0F),
                                 std::memory_order_relaxed);
    ++write_index;
  }

  self->waveform_write_index_.store(write_index, std::memory_order_release);
}

} // namespace analogno
