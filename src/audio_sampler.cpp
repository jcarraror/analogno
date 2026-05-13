#include "audio_sampler.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <utility>

namespace analogno {

AudioSampler::AudioSampler() {
  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_f32;
  config.playback.channels = channels;
  config.sampleRate = sample_rate;
  config.dataCallback = &AudioSampler::playback_callback;
  config.pUserData = this;

  if (ma_device_init(nullptr, &config, &device_) != MA_SUCCESS) {
    std::cerr << "warning: failed to initialize sampler playback device\n";
    return;
  }

  device_ready_ = true;

  if (ma_device_start(&device_) != MA_SUCCESS) {
    std::cerr << "warning: failed to start sampler playback device\n";
    ma_device_uninit(&device_);
    device_ready_ = false;
    return;
  }

  running_ = true;
  std::cout << "sampler playback device started: " << device_.playback.name
            << '\n';
}

AudioSampler::~AudioSampler() {
  if (device_ready_) {
    ma_device_uninit(&device_);
  }
}

auto AudioSampler::set_sample(std::vector<float> sample) -> void {
  const auto lock = std::scoped_lock{mutex_};
  sample_ = std::move(sample);
  trim_start_ = 0.0F;
  trim_end_ = 1.0F;

  for (auto &voice : voices_) {
    voice = {};
  }
}

auto AudioSampler::clear_sample() -> void {
  const auto lock = std::scoped_lock{mutex_};
  sample_.clear();
  trim_start_ = 0.0F;
  trim_end_ = 1.0F;

  for (auto &voice : voices_) {
    voice = {};
  }
}

auto AudioSampler::set_trim(float start, float end) -> void {
  const auto lock = std::scoped_lock{mutex_};
  const auto safe_start = std::clamp(start, 0.0F, 1.0F);
  const auto safe_end = std::clamp(end, 0.0F, 1.0F);

  trim_start_ = std::min(safe_start, safe_end - 0.001F);
  trim_end_ = std::max(safe_end, trim_start_ + 0.001F);
  trim_start_ = std::clamp(trim_start_, 0.0F, 1.0F);
  trim_end_ = std::clamp(trim_end_, trim_start_, 1.0F);

  for (auto &voice : voices_) {
    voice = {};
  }
}

auto AudioSampler::set_gain(float gain) -> void {
  const auto lock = std::scoped_lock{mutex_};
  gain_ = std::clamp(gain, 0.0F, 1.0F);
}

auto AudioSampler::set_pitch_controls(float pitch_bend,
                                      float vibrato_depth) -> void {
  const auto lock = std::scoped_lock{mutex_};
  pitch_bend_ = std::clamp(pitch_bend, -1.0F, 1.0F);
  vibrato_depth_ = std::clamp(vibrato_depth, 0.0F, 1.0F);
}

auto AudioSampler::trigger(float rate) -> void {
  const auto lock = std::scoped_lock{mutex_};

  if (sample_.empty()) {
    return;
  }

  auto &voice = voices_[next_voice_];
  voice = Voice{
      .active = true,
      .position = trim_start_ * static_cast<float>(sample_.size()),
      .rate = std::clamp(rate, 0.25F, 4.0F),
  };

  next_voice_ = (next_voice_ + 1) % voices_.size();
}

auto AudioSampler::has_sample() const -> bool {
  const auto lock = std::scoped_lock{mutex_};
  return !sample_.empty();
}

auto AudioSampler::sample_frames() const -> std::size_t {
  const auto lock = std::scoped_lock{mutex_};
  return sample_.size();
}

auto AudioSampler::trim_start() const -> float {
  const auto lock = std::scoped_lock{mutex_};
  return trim_start_;
}

auto AudioSampler::trim_end() const -> float {
  const auto lock = std::scoped_lock{mutex_};
  return trim_end_;
}

auto AudioSampler::is_running() const -> bool { return running_; }

auto AudioSampler::playback_callback(ma_device *device, void *output,
                                     const void *input, ma_uint32 frame_count)
    -> void {
  static_cast<void>(input);

  auto *self = static_cast<AudioSampler *>(device->pUserData);
  auto *out = static_cast<float *>(output);

  std::fill(out, out + static_cast<std::size_t>(frame_count) * channels, 0.0F);

  if (self == nullptr || !self->mutex_.try_lock()) {
    return;
  }

  if (self->sample_.empty()) {
    self->mutex_.unlock();
    return;
  }

  const auto trim_end =
      self->trim_end_ * static_cast<float>(self->sample_.size());
  constexpr auto pitch_bend_range_semitones = 2.0F;
  constexpr auto vibrato_range_semitones = 0.75F;
  constexpr auto vibrato_frequency = 6.0F;
  constexpr auto pi = std::numbers::pi_v<float>;

  for (ma_uint32 frame = 0; frame < frame_count; ++frame) {
    auto mixed = 0.0F;
    const auto vibrato =
        std::sin(self->vibrato_phase_) * self->vibrato_depth_ *
        vibrato_range_semitones;
    const auto pitch_semitones =
        self->pitch_bend_ * pitch_bend_range_semitones + vibrato;
    const auto pitch_rate = std::pow(2.0F, pitch_semitones / 12.0F);

    for (auto &voice : self->voices_) {
      if (!voice.active) {
        continue;
      }

      const auto index = static_cast<std::size_t>(voice.position);

      if (index >= self->sample_.size() || voice.position >= trim_end) {
        voice.active = false;
        continue;
      }

      mixed += self->sample_[index] * self->gain_;
      voice.position += voice.rate * pitch_rate;
    }

    self->vibrato_phase_ +=
        2.0F * pi * vibrato_frequency / static_cast<float>(sample_rate);

    if (self->vibrato_phase_ >= 2.0F * pi) {
      self->vibrato_phase_ -= 2.0F * pi;
    }

    mixed = std::clamp(mixed, -1.0F, 1.0F);
    const auto output_index = static_cast<std::size_t>(frame) * channels;
    out[output_index] = mixed;
    out[output_index + 1] = mixed;
  }

  self->mutex_.unlock();
}

} // namespace analogno
