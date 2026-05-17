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

std::vector<float> normalize_wavetable(std::vector<float> samples) {
  if (samples.empty()) {
    return {};
  }

  float peak = 0.0F;
  for (float &sample : samples) {
    sample = std::isfinite(sample) ? std::clamp(sample, -1.0F, 1.0F) : 0.0F;
    peak = std::max(peak, std::abs(sample));
  }

  if (peak > 1.0e-4F && peak < 0.75F) {
    const auto gain = 0.75F / peak;
    for (float &sample : samples) {
      sample = std::clamp(sample * gain, -1.0F, 1.0F);
    }
  }

  return samples;
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
  bank_morph_banks_[bank].reset();
  bank_trim_start_[bank].store(0.0F);
  bank_trim_end_[bank].store(1.0F);
  bank_is_wavetable_[bank].store(false);

  for (auto &voice : voices_) {
    if (voice.bank_index == bank) {
      voice = {};
    }
  }
  voice_generation_.fetch_add(1, std::memory_order_relaxed);
}

void AudioSampler::set_wavetable(std::vector<float> samples,
                                 std::vector<float> morph_samples) {
  const auto bank = active_bank_.load();
  const auto lock = std::scoped_lock{mutex_};
  banks_[bank] = std::make_shared<const std::vector<float>>(
      normalize_wavetable(std::move(samples)));
  if (!morph_samples.empty()) {
    bank_morph_banks_[bank] = std::make_shared<const std::vector<float>>(
        normalize_wavetable(std::move(morph_samples)));
  } else {
    bank_morph_banks_[bank].reset();
  }
  bank_trim_start_[bank].store(0.0F);
  bank_trim_end_[bank].store(1.0F);
  bank_is_wavetable_[bank].store(true);

  for (auto &voice : voices_) {
    if (voice.bank_index == bank) {
      voice = {};
    }
  }
  voice_generation_.fetch_add(1, std::memory_order_relaxed);
}

void AudioSampler::clear_sample() {
  const auto bank = active_bank_.load();
  const auto lock = std::scoped_lock{mutex_};
  banks_[bank].reset();
  bank_morph_banks_[bank].reset();
  bank_trim_start_[bank].store(0.0F);
  bank_trim_end_[bank].store(1.0F);
  bank_is_wavetable_[bank].store(false);

  for (auto &voice : voices_) {
    if (voice.bank_index == bank) {
      voice = {};
    }
  }
  voice_generation_.fetch_add(1, std::memory_order_relaxed);
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
  voice_generation_.fetch_add(1, std::memory_order_relaxed);
}

void AudioSampler::set_gain(float gain) {
  gain_.store(std::clamp(gain, 0.0F, 1.5F));
}

void AudioSampler::set_wavetable_controls(float morph, float noise,
                                          float unison) {
  wavetable_morph_.store(std::clamp(morph, 0.0F, 1.0F));
  wavetable_noise_.store(std::clamp(noise, 0.0F, 1.0F));
  wavetable_unison_.store(std::clamp(unison, 0.0F, 1.0F));
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

  const auto clamped_rate = std::clamp(rate, 0.25F, 4.0F);
  const auto trim_pos =
      bank_trim_start_[bank].load() * static_cast<float>(banks_[bank]->size());
  const auto is_wavetable = bank_is_wavetable_[bank].load();

  // Choke-on-retrigger: if a voice is already playing this bank at the same
  // pitch, restart it in place rather than layering a new one.  This gives
  // mono behaviour for repeated notes while still allowing polyphony across
  // different pitches (different rates).
  for (auto &voice : voices_) {
    if (voice.active && voice.bank_index == bank &&
        std::abs(voice.rate - clamped_rate) < 0.005F) {
      voice.releasing = false;
      voice.position = trim_pos;
      voice.envelope = 0.0F;
      return;
    }
  }

  auto &voice = voices_[next_voice_];
  const auto voice_index = next_voice_;
  voice = Voice{
      .active = true,
      .releasing = false,
      .loop = is_wavetable,
      .position = trim_pos,
      .rate = clamped_rate,
      .envelope = 0.0F,
      .bank_index = bank,
      .noise_state = static_cast<std::uint32_t>(
          0x9E3779B9U ^ ((bank + 1U) * 0x85EBCA6BU) ^
          ((voice_index + 1U) * 0xC2B2AE35U)),
  };

  next_voice_ = (next_voice_ + 1) % voices_.size();
}

void AudioSampler::release(float rate) {
  const auto clamped_rate = std::clamp(rate, 0.25F, 4.0F);
  const auto lock = std::scoped_lock{mutex_};

  for (auto &voice : voices_) {
    if (voice.active && !voice.releasing &&
        std::abs(voice.rate - clamped_rate) < 0.005F) {
      // One-shot samples (mic recordings) play to their natural trim_end and
      // release themselves — ignore note_off so the full sample is heard.
      // Looping voices (wavetables) are gate-controlled: stop on note_off.
      if (voice.loop) {
        voice.releasing = true;
      }
    }
  }
}

void AudioSampler::stop_all() {
  const auto lock = std::scoped_lock{mutex_};

  for (auto &voice : voices_) {
    voice = {};
  }
  voice_generation_.fetch_add(1, std::memory_order_relaxed);
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

std::vector<float> AudioSampler::sample_waveform(std::size_t n_points) const {
  const auto bank = active_bank_.load();
  const auto lock = std::scoped_lock{mutex_};
  const auto &ptr = banks_[bank];
  if (!ptr || ptr->empty() || n_points == 0) {
    return std::vector<float>(n_points, 0.0F);
  }
  const auto &raw = *ptr;
  // Stereo interleaved: each frame has `channels` floats.
  const auto frame_count = raw.size() / static_cast<std::size_t>(channels);
  std::vector<float> result(n_points, 0.0F);
  for (std::size_t i = 0; i < n_points; ++i) {
    const auto f0 = (i * frame_count) / n_points;
    const auto f1 = ((i + 1U) * frame_count) / n_points;
    float peak = 0.0F;
    for (auto f = f0; f < f1; ++f) {
      for (ma_uint32 c = 0; c < channels; ++c) {
        peak = std::max(peak, std::abs(raw[f * static_cast<std::size_t>(channels) + c]));
      }
    }
    result[i] = std::isfinite(peak) ? std::clamp(peak, 0.0F, 1.0F) : 0.0F;
  }
  return result;
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

bool AudioSampler::bank_is_wavetable(std::size_t bank) const {
  if (bank >= bank_count) {
    return false;
  }
  return bank_is_wavetable_[bank].load();
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
  std::array<std::shared_ptr<const std::vector<float>>, bank_count> bank_morph_banks{};
  std::array<Voice, voice_count> voices{};
  std::uint64_t voice_generation{};

  {
    const auto lock = std::scoped_lock{self->mutex_};
    banks = self->banks_;
    bank_morph_banks = self->bank_morph_banks_;
    voices = self->voices_;
    voice_generation =
        self->voice_generation_.load(std::memory_order_relaxed);
  }

  // Pre-compute trim boundaries (in samples) for each bank
  std::array<float, bank_count> trim_starts{};
  std::array<float, bank_count> trim_ends{};
  for (std::size_t b = 0; b < bank_count; ++b) {
    const auto sz = banks[b] ? static_cast<float>(banks[b]->size()) : 0.0F;
    trim_starts[b] = self->bank_trim_start_[b].load() * sz;
    trim_ends[b] = self->bank_trim_end_[b].load() * sz;
  }

  const auto gain = self->gain_.load();
  const auto wavetable_morph = self->wavetable_morph_.load();
  const auto wavetable_noise = self->wavetable_noise_.load();
  const auto wavetable_unison = self->wavetable_unison_.load();
  const auto pitch_bend = self->pitch_bend_.load();
  const auto vibrato_depth = self->vibrato_depth_.load();
  constexpr auto pitch_bend_range_semitones = 2.0F;
  constexpr auto vibrato_range_semitones = 0.75F;
  constexpr auto vibrato_frequency = 6.0F;
  constexpr auto pi = std::numbers::pi_v<float>;

  for (ma_uint32 frame = 0; frame < frame_count; ++frame) {
    auto mixed_left = 0.0F;
    auto mixed_right = 0.0F;
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
        if (voice.loop) {
          voice.position = trim_starts[voice.bank_index];
        } else {
          voice.releasing = true;
        }
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

      auto voice_sample = voice.loop ? sample_at_loop(*sample, voice.position)
                                     : sample_at(*sample, voice.position);
      auto voice_left = voice_sample;
      auto voice_right = voice_sample;
      const auto &morph_sample = bank_morph_banks[voice.bank_index];
      const auto has_morph = voice.loop && morph_sample && !morph_sample->empty();

      if (has_morph) {
        const auto target = sample_at_loop(*morph_sample, voice.position);
        voice_sample = voice_sample + (target - voice_sample) * wavetable_morph;
        voice_left = voice_sample;
        voice_right = voice_sample;
      }

      const auto unison_depth = std::sqrt(wavetable_unison);
      if (unison_depth > 0.001F) {
        constexpr auto detune_cents = 72.0F;
        const auto spread =
            std::pow(2.0F, (detune_cents * unison_depth) / 1200.0F);
        auto lower = voice.loop
                         ? sample_at_loop(*sample, voice.position / spread)
                         : sample_at(*sample, voice.position / spread);
        auto upper = voice.loop
                         ? sample_at_loop(*sample, voice.position * spread)
                         : sample_at(*sample, voice.position * spread);
        if (has_morph) {
          const auto lower_target =
              sample_at_loop(*morph_sample, voice.position / spread);
          const auto upper_target =
              sample_at_loop(*morph_sample, voice.position * spread);
          lower = lower + (lower_target - lower) * wavetable_morph;
          upper = upper + (upper_target - upper) * wavetable_morph;
        }
        const auto center_gain = 1.0F - 0.58F * unison_depth;
        const auto side_gain = 0.58F * unison_depth;
        voice_left = voice_sample * center_gain + lower * side_gain;
        voice_right = voice_sample * center_gain + upper * side_gain;
      }

      if (wavetable_noise > 0.001F) {
        const auto noise_left = noise_sample(voice.noise_state);
        const auto noise_right = noise_sample(voice.noise_state);
        voice_left = voice_left * (1.0F - wavetable_noise * 0.65F) +
                     noise_left * (wavetable_noise * 0.65F);
        voice_right = voice_right * (1.0F - wavetable_noise * 0.65F) +
                      noise_right * (wavetable_noise * 0.65F);
      }

      if (voice.loop) {
        if (voice.position >= trim_ends[voice.bank_index]) {
          voice.position = trim_starts[voice.bank_index];
        } else {
          voice.position += voice.rate * pitch_rate;
        }
      } else {
        voice.position += voice.rate * pitch_rate;
      }

      mixed_left += voice_left * voice.envelope * gain;
      mixed_right += voice_right * voice.envelope * gain;
    }

    self->vibrato_phase_ +=
        2.0F * pi * vibrato_frequency / static_cast<float>(sample_rate);

    if (self->vibrato_phase_ >= 2.0F * pi) {
      self->vibrato_phase_ -= 2.0F * pi;
    }

    mixed_left = std::clamp(mixed_left, -1.0F, 1.0F);
    mixed_right = std::clamp(mixed_right, -1.0F, 1.0F);
    const auto output_index = static_cast<std::size_t>(frame) * channels;
    out[output_index] = mixed_left;
    out[output_index + 1] = mixed_right;
  }

  {
    const auto lock = std::scoped_lock{self->mutex_};
    if (self->voice_generation_.load(std::memory_order_relaxed) ==
        voice_generation) {
      self->voices_ = voices;
    }
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

float AudioSampler::sample_at_loop(const std::vector<float> &sample,
                                   float position) {
  if (sample.empty()) {
    return 0.0F;
  }

  const auto size = static_cast<float>(sample.size());
  auto wrapped = std::fmod(position, size);
  if (wrapped < 0.0F) {
    wrapped += size;
  }

  const auto lower_index = static_cast<std::size_t>(wrapped);
  const auto upper_index = (lower_index + 1U) % sample.size();
  const auto fraction = wrapped - static_cast<float>(lower_index);
  const auto lower = sample[lower_index];
  const auto upper = sample[upper_index];

  return lower + (upper - lower) * fraction;
}

float AudioSampler::noise_sample(std::uint32_t &state) {
  if (state == 0) {
    state = 1;
  }

  state ^= state << 13U;
  state ^= state >> 17U;
  state ^= state << 5U;

  constexpr auto scale = 1.0F / 2147483648.0F;
  return (static_cast<float>(state & 0x7FFFFFFFU) * scale) * 2.0F - 1.0F;
}

} // namespace analogno
