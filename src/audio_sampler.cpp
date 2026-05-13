#include "audio_sampler.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <utility>

namespace analogno {
namespace {

void write_u16le(std::ostream &out, std::uint16_t v) {
  const std::array<std::uint8_t, 2> b{
      static_cast<std::uint8_t>(v & 0xFFU),
      static_cast<std::uint8_t>((v >> 8U) & 0xFFU),
  };
  out.write(reinterpret_cast<const char *>(b.data()), 2);
}

void write_u32le(std::ostream &out, std::uint32_t v) {
  const std::array<std::uint8_t, 4> b{
      static_cast<std::uint8_t>(v & 0xFFU),
      static_cast<std::uint8_t>((v >> 8U) & 0xFFU),
      static_cast<std::uint8_t>((v >> 16U) & 0xFFU),
      static_cast<std::uint8_t>((v >> 24U) & 0xFFU),
  };
  out.write(reinterpret_cast<const char *>(b.data()), 4);
}

bool write_wav_f32_mono(const std::string &path,
                        const std::vector<float> &samples,
                        std::uint32_t sample_rate) {
  namespace fs = std::filesystem;

  const auto parent = fs::path{path}.parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) {
      return false;
    }
  }

  std::ofstream file{path, std::ios::binary};
  if (!file) {
    return false;
  }

  const auto num_samples = static_cast<std::uint32_t>(samples.size());
  const auto data_bytes = num_samples * static_cast<std::uint32_t>(sizeof(float));
  const auto riff_data_size = 36U + data_bytes;

  file.write("RIFF", 4);
  write_u32le(file, riff_data_size);
  file.write("WAVE", 4);

  file.write("fmt ", 4);
  write_u32le(file, 16);        // chunk size
  write_u16le(file, 3);         // IEEE_FLOAT
  write_u16le(file, 1);         // mono
  write_u32le(file, sample_rate);
  write_u32le(file, sample_rate * 4); // byte rate
  write_u16le(file, 4);         // block align
  write_u16le(file, 32);        // bits per sample

  file.write("data", 4);
  write_u32le(file, data_bytes);
  file.write(reinterpret_cast<const char *>(samples.data()),
             static_cast<std::streamsize>(data_bytes));

  return file.good();
}

} // namespace

AudioSampler::AudioSampler() {
  for (auto &t : bank_trim_end_) {
    t.store(1.0F);
  }
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
  const auto bank = active_bank_.load();
  const auto lock = std::scoped_lock{mutex_};
  banks_[bank] = std::make_shared<const std::vector<float>>(std::move(sample));
  bank_trim_start_[bank].store(0.0F);
  bank_trim_end_[bank].store(1.0F);

  for (auto &voice : voices_) {
    if (voice.bank_index == bank) {
      voice = {};
    }
  }
}

void AudioSampler::clear_sample() {
  const auto bank = active_bank_.load();
  const auto lock = std::scoped_lock{mutex_};
  banks_[bank].reset();
  bank_trim_start_[bank].store(0.0F);
  bank_trim_end_[bank].store(1.0F);

  for (auto &voice : voices_) {
    if (voice.bank_index == bank) {
      voice = {};
    }
  }
}

void AudioSampler::set_trim(float start, float end) {
  const auto bank = active_bank_.load();
  const auto safe_start = std::clamp(start, 0.0F, 1.0F);
  const auto safe_end = std::clamp(end, 0.0F, 1.0F);

  auto trim_start = std::min(safe_start, safe_end - 0.001F);
  auto trim_end = std::max(safe_end, trim_start + 0.001F);
  trim_start = std::clamp(trim_start, 0.0F, 1.0F);
  trim_end = std::clamp(trim_end, trim_start, 1.0F);
  bank_trim_start_[bank].store(trim_start);
  bank_trim_end_[bank].store(trim_end);

  const auto lock = std::scoped_lock{mutex_};
  for (auto &voice : voices_) {
    if (voice.bank_index == bank) {
      voice = {};
    }
  }
}

void AudioSampler::set_gain(float gain) {
  gain_.store(std::clamp(gain, 0.0F, 1.0F));
}

void AudioSampler::set_pitch_controls(float pitch_bend, float vibrato_depth) {
  pitch_bend_.store(std::clamp(pitch_bend, -1.0F, 1.0F));
  vibrato_depth_.store(std::clamp(vibrato_depth, 0.0F, 1.0F));
}

void AudioSampler::set_active_bank(std::size_t bank) {
  if (bank < bank_count) {
    active_bank_.store(bank);
  }
}

void AudioSampler::trigger(float rate) {
  const auto bank = active_bank_.load();
  const auto lock = std::scoped_lock{mutex_};

  if (!banks_[bank] || banks_[bank]->empty()) {
    return;
  }

  auto &voice = voices_[next_voice_];
  voice = Voice{
      .active = true,
      .releasing = false,
      .position =
          bank_trim_start_[bank].load() * static_cast<float>(banks_[bank]->size()),
      .rate = std::clamp(rate, 0.25F, 4.0F),
      .envelope = 0.0F,
      .bank_index = bank,
  };

  next_voice_ = (next_voice_ + 1) % voices_.size();
}

bool AudioSampler::save_bank(std::size_t bank, const std::string &path) {
  if (bank >= bank_count) {
    return false;
  }

  std::shared_ptr<const std::vector<float>> sample{};
  {
    const auto lock = std::scoped_lock{mutex_};
    sample = banks_[bank];
  }

  if (!sample || sample->empty()) {
    return false;
  }

  return write_wav_f32_mono(path, *sample, sample_rate);
}

bool AudioSampler::has_sample() const {
  const auto bank = active_bank_.load();
  const auto lock = std::scoped_lock{mutex_};
  return banks_[bank] && !banks_[bank]->empty();
}

std::size_t AudioSampler::sample_frames() const {
  const auto bank = active_bank_.load();
  const auto lock = std::scoped_lock{mutex_};
  return banks_[bank] ? banks_[bank]->size() : 0U;
}

float AudioSampler::trim_start() const {
  return bank_trim_start_[active_bank_.load()].load();
}

float AudioSampler::trim_end() const {
  return bank_trim_end_[active_bank_.load()].load();
}

std::size_t AudioSampler::active_bank() const {
  return active_bank_.load();
}

bool AudioSampler::bank_has_sample(std::size_t bank) const {
  if (bank >= bank_count) {
    return false;
  }
  const auto lock = std::scoped_lock{mutex_};
  return banks_[bank] && !banks_[bank]->empty();
}

std::size_t AudioSampler::bank_frames(std::size_t bank) const {
  if (bank >= bank_count) {
    return 0U;
  }
  const auto lock = std::scoped_lock{mutex_};
  return banks_[bank] ? banks_[bank]->size() : 0U;
}

float AudioSampler::bank_trim_start(std::size_t bank) const {
  if (bank >= bank_count) {
    return 0.0F;
  }
  return bank_trim_start_[bank].load();
}

float AudioSampler::bank_trim_end(std::size_t bank) const {
  if (bank >= bank_count) {
    return 1.0F;
  }
  return bank_trim_end_[bank].load();
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

  std::array<std::shared_ptr<const std::vector<float>>, bank_count> banks{};
  std::array<Voice, voice_count> voices{};

  {
    const auto lock = std::scoped_lock{self->mutex_};
    banks = self->banks_;
    voices = self->voices_;
  }

  // Pre-compute trim end boundaries (samples * trim fraction) for each bank
  std::array<float, bank_count> trim_ends{};
  for (std::size_t b = 0; b < bank_count; ++b) {
    const auto sz = banks[b] ? static_cast<float>(banks[b]->size()) : 0.0F;
    trim_ends[b] = self->bank_trim_end_[b].load() * sz;
  }

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

      const auto &sample = banks[voice.bank_index];
      if (!sample || sample->empty()) {
        voice.active = false;
        continue;
      }

      const auto trim_end = trim_ends[voice.bank_index];
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
