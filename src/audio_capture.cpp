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
constexpr auto gate_open_threshold = 0.12F;
constexpr auto gate_close_threshold = 0.06F;
constexpr auto onset_cooldown_frames = ma_uint32{4800};

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

const std::vector<CaptureDeviceInfo> &AudioCapture::devices() const {
  return devices_;
}

void AudioCapture::start(std::optional<std::uint32_t> preferred_index) {
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

void AudioCapture::stop() {
  if (!device_ready_) {
    return;
  }

  ma_device_uninit(&device_);
  device_ready_ = false;
  running_ = false;
  selected_device_name_ = "none";
  selected_device_index_.reset();
  level_.store(0.0F);
  envelope_.store(0.0F);
  gate_open_.store(false);
  onset_count_.store(0);
  onset_velocity_.store(0);
  captured_sample_frames_.store(0);
  sample_recording_.store(false);
  onset_cooldown_frames_ = 0;

  for (auto &sample : waveform_) {
    sample.store(0.0F);
  }

  waveform_write_index_.store(0);

  {
    const auto lock = std::scoped_lock{sample_mutex_};
    recording_sample_.clear();
    captured_sample_.clear();
  }
}

bool AudioCapture::is_running() const { return running_; }

void AudioCapture::begin_sample_recording() {
  const auto lock = std::scoped_lock{sample_mutex_};
  recording_sample_.clear();
  recording_sample_.reserve(max_sample_frames);
  sample_recording_.store(true);
  captured_sample_frames_.store(0);
}

void AudioCapture::end_sample_recording() {
  const auto lock = std::scoped_lock{sample_mutex_};
  sample_recording_.store(false);

  if (!recording_sample_.empty()) {
    captured_sample_ = recording_sample_;
    captured_sample_frames_.store(captured_sample_.size());
  }

  recording_sample_.clear();
}

bool AudioCapture::is_sample_recording() const {
  return sample_recording_.load();
}

float AudioCapture::level() const { return level_.load(); }

AudioFeatures AudioCapture::consume_features() {
  const auto level = level_.load();

  return AudioFeatures{
      .level = level,
      .envelope = envelope_.load(),
      .gate_open = gate_open_.load(),
      .onset = onset_count_.exchange(0) > 0,
      .velocity = onset_velocity_.load(),
  };
}

std::vector<float> AudioCapture::waveform() const {
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

std::optional<std::vector<float>> AudioCapture::consume_captured_sample() {
  const auto lock = std::scoped_lock{sample_mutex_};

  if (captured_sample_.empty()) {
    return std::nullopt;
  }

  auto sample = std::move(captured_sample_);
  captured_sample_.clear();
  return sample;
}

std::size_t AudioCapture::captured_sample_frames() const {
  return captured_sample_frames_.load();
}

const std::string &AudioCapture::selected_device_name() const {
  return selected_device_name_;
}

std::optional<std::uint32_t> AudioCapture::selected_device_index() const {
  return selected_device_index_;
}

void AudioCapture::enumerate_devices() {
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

std::optional<ma_device_id>
AudioCapture::choose_device(std::optional<std::uint32_t> preferred_index) const {
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

std::optional<std::uint32_t> AudioCapture::default_device_index() const {
  for (const auto &device : devices_) {
    if (device.is_default) {
      return device.index;
    }
  }

  return std::nullopt;
}

void AudioCapture::capture_callback(ma_device *device, void *output,
                                    const void *input,
                                    ma_uint32 frame_count) {
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
  const auto normalized_level = std::clamp(smoothed * 8.0F, 0.0F, 1.0F);

  self->level_.store(normalized_level);
  self->envelope_.store(smoothed);

  if (self->onset_cooldown_frames_ > frame_count) {
    self->onset_cooldown_frames_ -= frame_count;
  } else {
    self->onset_cooldown_frames_ = 0;
  }

  const auto was_gate_open = self->gate_open_.load();
  auto gate_open = was_gate_open;

  if (!gate_open && normalized_level >= gate_open_threshold) {
    gate_open = true;
  } else if (gate_open && normalized_level <= gate_close_threshold) {
    gate_open = false;
  }

  self->gate_open_.store(gate_open);

  if (gate_open && !was_gate_open && self->onset_cooldown_frames_ == 0) {
    const auto velocity = 1 + static_cast<int>(std::lround(normalized_level * 126.0F));
    self->onset_velocity_.store(std::clamp(velocity, 1, 127));
    self->onset_count_.fetch_add(1);
    self->onset_cooldown_frames_ = onset_cooldown_frames;
  }

  auto write_index =
      self->waveform_write_index_.load(std::memory_order_relaxed);

  for (std::size_t i = 0; i < sample_count; ++i) {
    const auto index = write_index % waveform_sample_count;
    const auto sample = std::clamp(samples[i], -1.0F, 1.0F);
    self->waveform_[index].store(sample,
                                 std::memory_order_relaxed);
    ++write_index;
  }

  self->waveform_write_index_.store(write_index, std::memory_order_release);

  if (self->sample_recording_.load() && self->sample_mutex_.try_lock()) {
    const auto remaining =
        max_sample_frames > self->recording_sample_.size()
            ? max_sample_frames - self->recording_sample_.size()
            : std::size_t{0};
    const auto copy_count = std::min(sample_count, remaining);

    self->recording_sample_.insert(self->recording_sample_.end(), samples,
                                   samples + copy_count);
    self->sample_mutex_.unlock();
  }
}

} // namespace analogno
