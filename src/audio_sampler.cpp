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

void AudioSampler::set_sample(std::vector<float> sample) {
  const auto lock = std::scoped_lock{mutex_};
  sample_ = std::make_shared<const std::vector<float>>(std::move(sample));
  trim_start_.store(0.0F);
  trim_end_.store(1.0F);

  for (auto &voice : voices_) {
    voice = {};
  }
}

void AudioSampler::clear_sample() {
  const auto lock = std::scoped_lock{mutex_};
  sample_.reset();
  trim_start_.store(0.0F);
  trim_end_.store(1.0F);

  for (auto &voice : voices_) {
    voice = {};
  }
}

void AudioSampler::set_trim(float start, float end) {
  const auto lock = std::scoped_lock{mutex_};
  const auto safe_start = std::clamp(start, 0.0F, 1.0F);
  const auto safe_end = std::clamp(end, 0.0F, 1.0F);

  auto trim_start = std::min(safe_start, safe_end - 0.001F);
  auto trim_end = std::max(safe_end, trim_start + 0.001F);
  trim_start = std::clamp(trim_start, 0.0F, 1.0F);
  trim_end = std::clamp(trim_end, trim_start, 1.0F);
  trim_start_.store(trim_start);
  trim_end_.store(trim_end);

  for (auto &voice : voices_) {
    voice = {};
  }
}

void AudioSampler::set_gain(float gain) {
  const auto lock = std::scoped_lock{mutex_};
  gain_.store(std::clamp(gain, 0.0F, 1.0F));
}

void AudioSampler::set_pitch_controls(float pitch_bend, float vibrato_depth) {
  const auto lock = std::scoped_lock{mutex_};
  pitch_bend_.store(std::clamp(pitch_bend, -1.0F, 1.0F));
  vibrato_depth_.store(std::clamp(vibrato_depth, 0.0F, 1.0F));
}

void AudioSampler::trigger(float rate) {
  const auto lock = std::scoped_lock{mutex_};

  if (!sample_ || sample_->empty()) {
    return;
  }

  auto &voice = voices_[next_voice_];
  voice = Voice{
      .active = true,
      .releasing = false,
      .position =
          trim_start_.load() * static_cast<float>(sample_->size()),
      .rate = std::clamp(rate, 0.25F, 4.0F),
      .envelope = 0.0F,
  };

  next_voice_ = (next_voice_ + 1) % voices_.size();
}

bool AudioSampler::has_sample() const {
  const auto lock = std::scoped_lock{mutex_};
  return sample_ && !sample_->empty();
}

std::size_t AudioSampler::sample_frames() const {
  const auto lock = std::scoped_lock{mutex_};
  return sample_ ? sample_->size() : 0U;
}

float AudioSampler::trim_start() const {
  return trim_start_.load();
}

float AudioSampler::trim_end() const {
  return trim_end_.load();
}

bool AudioSampler::is_running() const { return running_; }

void AudioSampler::playback_callback(ma_device *device, void *output,
                                     const void *input,
                                     ma_uint32 frame_count) {
  static_cast<void>(input);

  auto *self = static_cast<AudioSampler *>(device->pUserData);
  auto *out = static_cast<float *>(output);

  std::fill(out, out + static_cast<std::size_t>(frame_count) * channels, 0.0F);

  if (self == nullptr) {
    return;
  }

  std::shared_ptr<const std::vector<float>> sample{};
  std::array<Voice, voice_count> voices{};

  {
    const auto lock = std::scoped_lock{self->mutex_};
    sample = self->sample_;
    voices = self->voices_;
  }

  if (!sample || sample->empty()) {
    return;
  }

  const auto trim_end =
      self->trim_end_.load() * static_cast<float>(sample->size());
  const auto gain = self->gain_.load();
  const auto pitch_bend = self->pitch_bend_.load();
  const auto vibrato_depth = self->vibrato_depth_.load();
  constexpr auto pitch_bend_range_semitones = 2.0F;
  constexpr auto vibrato_range_semitones = 0.75F;
  constexpr auto vibrato_frequency = 6.0F;
  constexpr auto pi = std::numbers::pi_v<float>;

  for (ma_uint32 frame = 0; frame < frame_count; ++frame) {
    auto mixed = 0.0F;
    const auto vibrato =
        std::sin(self->vibrato_phase_) * vibrato_depth * vibrato_range_semitones;
    const auto pitch_semitones =
        pitch_bend * pitch_bend_range_semitones + vibrato;
    const auto pitch_rate = std::pow(2.0F, pitch_semitones / 12.0F);

    for (auto &voice : voices) {
      if (!voice.active) {
        continue;
      }

      const auto index = static_cast<std::size_t>(voice.position);

      if (index >= sample->size() || voice.position >= trim_end) {
        voice.releasing = true;
      }

      if (voice.releasing) {
        voice.envelope =
            std::max(0.0F, voice.envelope - (1.0F / release_frames));
      } else {
        voice.envelope =
            std::min(1.0F, voice.envelope + (1.0F / attack_frames));
      }

      if (voice.envelope <= 0.0F) {
        voice.active = false;
        continue;
      }

      mixed += sample_at(*sample, voice.position) * voice.envelope * gain;
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

  {
    const auto lock = std::scoped_lock{self->mutex_};
    self->voices_ = voices;
  }
}

float AudioSampler::sample_at(const std::vector<float> &sample,
                              float position) {
  if (sample.empty()) {
    return 0.0F;
  }

  const auto lower_index = static_cast<std::size_t>(position);

  if (lower_index >= sample.size()) {
    return 0.0F;
  }

  const auto upper_index = std::min(lower_index + 1, sample.size() - 1);
  const auto fraction = position - static_cast<float>(lower_index);
  const auto lower = sample[lower_index];
  const auto upper = sample[upper_index];

  return lower + (upper - lower) * fraction;
}

} // namespace analogno
