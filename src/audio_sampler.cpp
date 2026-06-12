#include "audio_sampler.hpp"
#include "wav_stream.hpp"

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

bool write_wav_f32(const std::string &path,
                   const std::vector<float> &samples,
                   std::uint32_t channel_count,
                   std::uint32_t sample_rate) {
  namespace fs = std::filesystem;

  const auto parent = fs::path{path}.parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) return false;
  }

  std::ofstream file{path, std::ios::binary};
  if (!file) return false;

  const auto ch = std::max(channel_count, 1U);
  const auto block_align = ch * 4U;
  const auto byte_rate = sample_rate * block_align;
  const auto data_bytes = static_cast<std::uint32_t>(samples.size()) * 4U;
  const auto riff_data_size = 36U + data_bytes;

  file.write("RIFF", 4);
  write_u32le(file, riff_data_size);
  file.write("WAVE", 4);
  file.write("fmt ", 4);
  write_u32le(file, 16);
  write_u16le(file, 3);           // IEEE_FLOAT
  write_u16le(file, static_cast<std::uint16_t>(ch));
  write_u32le(file, sample_rate);
  write_u32le(file, byte_rate);
  write_u16le(file, static_cast<std::uint16_t>(block_align));
  write_u16le(file, 32);
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

AudioSampler::AudioSampler(ma_context *shared_ctx) {
  for (auto &ws : wav_streams_) {
    ws = std::make_unique<WavStream>();
  }
  for (auto &ss : stem_streams_) {
    ss = std::make_unique<WavStream>();
  }
  for (auto &c : bank_channels_) {
    c.store(1U);
  }
  for (auto &t : bank_trim_end_) {
    t.store(1.0F);
  }
  for (auto &rn : bank_root_note_) {
    rn.store(48);
  }
  for (auto &r : bank_playback_rate_) {
    r.store(1.0F);
  }
  for (auto &f : bank_stream_end_frac_) {
    f.store(1.0F);
  }
  for (auto &v : bank_waveform_ver_) {
    v.store(1U);
  }
  for (auto &v : stem_waveform_ver_) {
    v.store(1U);
  }
  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_f32;
  config.playback.channels = channels;
  config.sampleRate = sample_rate;
  config.dataCallback = &AudioSampler::playback_callback;
  config.pUserData = this;

  if (ma_device_init(shared_ctx, &config, &device_) != MA_SUCCESS) {
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
  set_sample_for_bank(active_bank_.load(), std::move(sample), 1U);
}

void AudioSampler::set_sample(std::vector<float> sample, std::uint32_t channel_count,
                              std::uint32_t src_rate) {
  set_sample_for_bank(active_bank_.load(), std::move(sample), channel_count, src_rate);
}

void AudioSampler::set_bank_file(std::size_t bank, std::string path) {
  if (bank >= bank_count) return;
  wav_streams_[bank]->stop();
  bank_wav_file_[bank] = std::move(path);
  bank_trim_start_[bank].store(0.0F);
  bank_trim_end_[bank].store(1.0F);
  bank_stream_[bank].store(true);

  // Read total frame count from file header (fast — just parses the WAV header).
  {
    auto cfg = ma_decoder_config_init(ma_format_f32, 2U, 48000U);
    ma_decoder dec{};
    ma_uint64 total_frames = 0;
    if (ma_decoder_init_file(bank_wav_file_[bank].c_str(), &cfg, &dec) == MA_SUCCESS) {
      ma_decoder_get_length_in_pcm_frames(&dec, &total_frames);
      ma_decoder_uninit(&dec);
    }
    bank_file_frames_[bank].store(total_frames);
  }

  // Clear old cached waveform immediately so stale data isn't shown.
  {
    const auto lock = std::scoped_lock{mutex_};
    bank_cached_waveforms_[bank].reset();
  }

  // Compute waveform asynchronously — file reads happen off the event-loop thread.
  const auto bpath = bank_wav_file_[bank];
  std::thread([this, bank, bpath] {
    auto wf = compute_file_waveform(bpath, 256U);
    const auto lock = std::scoped_lock{mutex_};
    bank_cached_waveforms_[bank] =
        std::make_shared<const std::vector<float>>(std::move(wf));
  }).detach();

  bank_file_backed_[bank].store(true, std::memory_order_release);
  bank_waveform_ver_[bank].fetch_add(1U, std::memory_order_relaxed);
}

void AudioSampler::set_sample_for_bank(std::size_t bank, std::vector<float> sample,
                                       std::uint32_t channel_count,
                                       std::uint32_t src_rate) {
  if (bank >= bank_count) return;
  wav_streams_[bank]->stop();
  bank_wav_file_[bank].clear();
  bank_file_backed_[bank].store(false, std::memory_order_release);
  bank_file_frames_[bank].store(0U);
  const auto safe_channels = channel_count == 2U ? 2U : 1U;
  if (safe_channels == 2U && sample.size() % 2U != 0U) {
    sample.pop_back();
  }
  const auto rate = src_rate > 0U
                        ? static_cast<float>(src_rate) / static_cast<float>(sample_rate)
                        : 1.0F;
  const auto lock = std::scoped_lock{mutex_};
  banks_[bank] = std::make_shared<const std::vector<float>>(std::move(sample));
  bank_cached_waveforms_[bank] =
      std::make_shared<const std::vector<float>>(compute_waveform(*banks_[bank], safe_channels, 256U));
  bank_morph_banks_[bank].reset();
  bank_channels_[bank].store(safe_channels);
  bank_playback_rate_[bank].store(rate);
  bank_trim_start_[bank].store(0.0F);
  bank_trim_end_[bank].store(1.0F);
  bank_is_wavetable_[bank].store(false);
  bank_stream_[bank].store(false);
  stream_active_[bank].store(false, std::memory_order_release);
  stream_pos_[bank].store(0, std::memory_order_relaxed);

  bank_onset_frames_[bank].clear();
  bank_waveform_ver_[bank].fetch_add(1U, std::memory_order_relaxed);

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
  wav_streams_[bank]->stop();
  bank_wav_file_[bank].clear();
  bank_file_backed_[bank].store(false, std::memory_order_release);
  bank_file_frames_[bank].store(0U);
  const auto lock = std::scoped_lock{mutex_};
  banks_[bank] = std::make_shared<const std::vector<float>>(
      normalize_wavetable(std::move(samples)));
  bank_cached_waveforms_[bank] =
      std::make_shared<const std::vector<float>>(compute_waveform(*banks_[bank], 1U, 256U));
  bank_channels_[bank].store(1U);
  if (!morph_samples.empty()) {
    bank_morph_banks_[bank] = std::make_shared<const std::vector<float>>(
        normalize_wavetable(std::move(morph_samples)));
  } else {
    bank_morph_banks_[bank].reset();
  }
  bank_trim_start_[bank].store(0.0F);
  bank_trim_end_[bank].store(1.0F);
  bank_is_wavetable_[bank].store(true);
  bank_stream_[bank].store(false);
  stream_active_[bank].store(false, std::memory_order_release);
  stream_pos_[bank].store(0, std::memory_order_relaxed);
  bank_waveform_ver_[bank].fetch_add(1U, std::memory_order_relaxed);

  for (auto &voice : voices_) {
    if (voice.bank_index == bank) {
      voice = {};
    }
  }
  voice_generation_.fetch_add(1, std::memory_order_relaxed);
}

void AudioSampler::clear_sample() {
  const auto bank = active_bank_.load();
  wav_streams_[bank]->stop();
  bank_wav_file_[bank].clear();
  bank_file_backed_[bank].store(false, std::memory_order_release);
  bank_file_frames_[bank].store(0U);
  const auto lock = std::scoped_lock{mutex_};
  banks_[bank].reset();
  bank_cached_waveforms_[bank].reset();
  bank_morph_banks_[bank].reset();
  bank_channels_[bank].store(1U);
  bank_trim_start_[bank].store(0.0F);
  bank_trim_end_[bank].store(1.0F);
  bank_is_wavetable_[bank].store(false);
  bank_stream_[bank].store(false);
  stream_active_[bank].store(false, std::memory_order_release);
  stream_pos_[bank].store(0, std::memory_order_relaxed);
  bank_waveform_ver_[bank].fetch_add(1U, std::memory_order_relaxed);

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

void AudioSampler::load_stem_to_bank(std::size_t bank, const std::string &path,
                                     float trim_start, float trim_end) {
  if (bank >= bank_count) return;
  for (std::size_t i = 0; i < stem_count; ++i) {
    if (stem_streams_[i]->is_active()) stem_streams_[i]->stop();
  }

  auto cfg = ma_decoder_config_init(ma_format_f32, 2U, 48000U);
  ma_decoder dec{};
  if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) return;

  ma_uint64 total = 0;
  ma_decoder_get_length_in_pcm_frames(&dec, &total);

  const auto start_f = static_cast<ma_uint64>(trim_start * static_cast<float>(total));
  const auto end_f   = static_cast<ma_uint64>(trim_end   * static_cast<float>(total));
  const auto n_frames = end_f > start_f ? end_f - start_f : total;

  constexpr ma_uint64 stream_threshold = 48000ULL * 30; // 30 seconds

  if (n_frames > stream_threshold) {
    ma_decoder_uninit(&dec);
    set_bank_file(bank, path);
    set_bank_trim(bank, trim_start, trim_end);
    return;
  }

  if (start_f > 0) ma_decoder_seek_to_pcm_frame(&dec, start_f);

  std::vector<float> pcm(static_cast<std::size_t>(n_frames) * 2);
  ma_uint64 frames_read = 0;
  ma_decoder_read_pcm_frames(&dec, pcm.data(), n_frames, &frames_read);
  ma_decoder_uninit(&dec);
  pcm.resize(static_cast<std::size_t>(frames_read) * 2);

  set_sample_for_bank(bank, std::move(pcm), 2U, 48000U);
}

void AudioSampler::set_bank_trim(std::size_t bank, float start, float end) {
  if (bank >= bank_count) return;
  const auto safe_start = std::clamp(start, 0.0F, 1.0F);
  const auto safe_end = std::clamp(end, 0.0F, 1.0F);
  auto trim_start = std::min(safe_start, safe_end - 0.001F);
  auto trim_end = std::max(safe_end, trim_start + 0.001F);
  trim_start = std::clamp(trim_start, 0.0F, 1.0F);
  trim_end = std::clamp(trim_end, trim_start, 1.0F);
  bank_trim_start_[bank].store(trim_start);
  bank_trim_end_[bank].store(trim_end);
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

void AudioSampler::trigger(float rate, int midi_note) {
  trigger_bank(active_bank_.load(), rate, midi_note);
}

void AudioSampler::trigger_bank(std::size_t bank, float rate, int midi_note, float velocity_gain, float pan) {
  if (bank >= bank_count) {
    return;
  }

  const auto lock = std::scoped_lock{mutex_};

  if (!banks_[bank] || banks_[bank]->empty()) {
    return;
  }

  const auto is_wavetable = bank_is_wavetable_[bank].load();
  const auto bank_channel_count = std::max<std::size_t>(bank_channels_[bank].load(), 1U);
  const auto total_frames = static_cast<double>(banks_[bank]->size() / bank_channel_count);
  const auto trim_start_f = static_cast<double>(bank_trim_start_[bank].load()) * total_frames;
  const auto trim_end_f = static_cast<double>(bank_trim_end_[bank].load()) * total_frames;

  const auto slice_count = bank_slice_count_[bank].load();
  const auto use_slice = slice_count > 0 && midi_note >= 0 && !is_wavetable;

  double voice_rate{};
  double start_pos{};
  double end_override{-1.0};
  int voice_slice_idx = -1;

  if (use_slice) {
    const auto root = bank_root_note_[bank].load();
    const auto sidx = std::clamp(midi_note - root, 0, slice_count - 1);
    voice_slice_idx = sidx;
    voice_rate = 1.0;
    const auto slice_len = (trim_end_f - trim_start_f) / static_cast<double>(slice_count);
    start_pos = trim_start_f + static_cast<double>(sidx) * slice_len;
    end_override = trim_start_f + static_cast<double>(sidx + 1) * slice_len;

    for (auto &voice : voices_) {
      if (voice.active && voice.bank_index == bank && voice.slice_idx == sidx) {
        voice.releasing = false;
        voice.position = start_pos;
        voice.trim_end_override = end_override;
        voice.envelope = 0.0F;
        voice.velocity_gain = std::clamp(velocity_gain, 0.0F, 1.0F);
        voice.pan = std::clamp(pan, -1.0F, 1.0F);
        voice_generation_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  } else {
    const auto clamped_rate = static_cast<double>(std::clamp(rate, 0.25F, 4.0F));
    voice_rate = clamped_rate;
    start_pos = trim_start_f;

    for (auto &voice : voices_) {
      if (voice.active && voice.bank_index == bank &&
          std::abs(voice.rate - clamped_rate) < 0.005) {
        voice.releasing = false;
        voice.position = start_pos;
        voice.trim_end_override = -1.0;
        voice.slice_idx = -1;
        voice.envelope = 0.0F;
        voice.velocity_gain = std::clamp(velocity_gain, 0.0F, 1.0F);
        voice.pan = std::clamp(pan, -1.0F, 1.0F);
        voice_generation_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }

  std::size_t best_idx = SIZE_MAX;
  float lowest_env = 2.0F;
  for (std::size_t i = 0; i < voices_.size(); ++i) {
    if (!voices_[i].active) { best_idx = i; break; }
    if (voices_[i].releasing && voices_[i].envelope < lowest_env) {
      lowest_env = voices_[i].envelope;
      best_idx = i;
    }
  }
  if (best_idx == SIZE_MAX) {
    best_idx = next_voice_;
    next_voice_ = (next_voice_ + 1) % voices_.size();
    perf_.voice_steals.fetch_add(1U, std::memory_order_relaxed);
  }
  const auto voice_index = best_idx;
  voices_[best_idx] = Voice{
      .active = true,
      .releasing = false,
      .loop = is_wavetable,
      .position = start_pos,
      .rate = voice_rate,
      .trim_end_override = end_override,
      .slice_idx = voice_slice_idx,
      .envelope = 0.0F,
      .velocity_gain = std::clamp(velocity_gain, 0.0F, 1.0F),
      .pan = std::clamp(pan, -1.0F, 1.0F),
      .bank_index = bank,
      .noise_state = static_cast<std::uint32_t>(
          0x9E3779B9U ^ ((bank + 1U) * 0x85EBCA6BU) ^
          ((voice_index + 1U) * 0xC2B2AE35U)),
  };
  voice_generation_.fetch_add(1, std::memory_order_relaxed);
}

void AudioSampler::release(float rate, int midi_note) {
  release_bank(active_bank_.load(), rate, midi_note);
}

void AudioSampler::release_bank(std::size_t bank, float rate, int midi_note) {
  if (bank >= bank_count) {
    return;
  }

  const auto lock = std::scoped_lock{mutex_};
  const auto slice_count = bank_slice_count_[bank].load();
  const auto use_slice = slice_count > 0 && midi_note >= 0;

  for (auto &voice : voices_) {
    if (!voice.active || voice.releasing || voice.bank_index != bank) {
      continue;
    }

    bool match;
    if (use_slice) {
      const auto root = bank_root_note_[bank].load();
      const auto sidx = std::clamp(midi_note - root, 0, slice_count - 1);
      match = voice.slice_idx == sidx;
    } else {
      const auto clamped_rate = static_cast<double>(std::clamp(rate, 0.25F, 4.0F));
      match = std::abs(voice.rate - clamped_rate) < 0.005;
    }

    if (match) {
      voice.releasing = true;
      voice_generation_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void AudioSampler::set_bank_root_note(std::size_t bank, int note) {
  if (bank < bank_count) {
    bank_root_note_[bank].store(std::clamp(note, 0, 127));
  }
}

int AudioSampler::bank_root_note(std::size_t bank) const {
  if (bank >= bank_count) return 48;
  return bank_root_note_[bank].load();
}

void AudioSampler::set_bank_slice_count(std::size_t bank, int count) {
  if (bank < bank_count) {
    bank_slice_count_[bank].store(std::clamp(count, 0, 64));
  }
}

int AudioSampler::bank_slice_count(std::size_t bank) const {
  if (bank >= bank_count) return 0;
  return bank_slice_count_[bank].load();
}

void AudioSampler::set_bank_onset_frames(std::size_t bank,
                                         std::vector<double> frame_positions) {
  if (bank >= bank_count) return;
  const auto lock = std::scoped_lock{mutex_};
  bank_onset_frames_[bank] = std::move(frame_positions);
}

void AudioSampler::clear_bank_onset_frames(std::size_t bank) {
  if (bank >= bank_count) return;
  const auto lock = std::scoped_lock{mutex_};
  bank_onset_frames_[bank].clear();
}

bool AudioSampler::bank_has_onsets(std::size_t bank) const {
  if (bank >= bank_count) return false;
  const auto lock = std::scoped_lock{mutex_};
  return !bank_onset_frames_[bank].empty();
}

void AudioSampler::trigger_bank_onset(std::size_t bank, int onset_idx, float velocity_gain, float pan) {
  if (bank >= bank_count) return;

  const auto lock = std::scoped_lock{mutex_};

  if (!banks_[bank] || banks_[bank]->empty()) return;
  if (bank_onset_frames_[bank].empty()) return;

  const auto &onsets = bank_onset_frames_[bank];
  const auto sidx = std::clamp(onset_idx, 0, static_cast<int>(onsets.size()) - 1);
  const auto start_pos = onsets[static_cast<std::size_t>(sidx)];
  const auto bank_channel_count = std::max<std::size_t>(bank_channels_[bank].load(), 1U);
  const auto total_frames = static_cast<double>(banks_[bank]->size() / bank_channel_count);
  const auto trim_end_pos = static_cast<double>(bank_trim_end_[bank].load()) * total_frames;
  const auto end_pos = (static_cast<std::size_t>(sidx) + 1U < onsets.size())
                           ? onsets[static_cast<std::size_t>(sidx) + 1U]
                           : trim_end_pos;

  for (auto &voice : voices_) {
    if (voice.active && voice.bank_index == bank && voice.slice_idx == sidx) {
      voice.releasing = false;
      voice.position = start_pos;
      voice.trim_end_override = end_pos;
      voice.envelope = 0.0F;
      voice.velocity_gain = std::clamp(velocity_gain, 0.0F, 1.0F);
      voice.pan           = std::clamp(pan, -1.0F, 1.0F);
      voice_generation_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }

  std::size_t best_idx_o = SIZE_MAX;
  float lowest_env_o = 2.0F;
  for (std::size_t i = 0; i < voices_.size(); ++i) {
    if (!voices_[i].active) { best_idx_o = i; break; }
    if (voices_[i].releasing && voices_[i].envelope < lowest_env_o) {
      lowest_env_o = voices_[i].envelope;
      best_idx_o = i;
    }
  }
  if (best_idx_o == SIZE_MAX) {
    best_idx_o = next_voice_;
    next_voice_ = (next_voice_ + 1) % voices_.size();
    perf_.voice_steals.fetch_add(1U, std::memory_order_relaxed);
  }
  const auto voice_index = best_idx_o;
  voices_[best_idx_o] = Voice{
      .active = true,
      .releasing = false,
      .loop = false,
      .position = start_pos,
      .rate = 1.0,
      .trim_end_override = end_pos,
      .slice_idx = sidx,
      .envelope = 0.0F,
      .velocity_gain = std::clamp(velocity_gain, 0.0F, 1.0F),
      .pan = std::clamp(pan, -1.0F, 1.0F),
      .bank_index = bank,
      .noise_state = static_cast<std::uint32_t>(
          0x9E3779B9U ^ ((bank + 1U) * 0x85EBCA6BU) ^
          ((voice_index + 1U) * 0xC2B2AE35U)),
  };
  voice_generation_.fetch_add(1, std::memory_order_relaxed);
}

void AudioSampler::release_bank_onset(std::size_t bank, int onset_idx) {
  if (bank >= bank_count) return;
  const auto lock = std::scoped_lock{mutex_};
  if (bank_onset_frames_[bank].empty()) return;
  const auto sidx = std::clamp(onset_idx, 0, static_cast<int>(bank_onset_frames_[bank].size()) - 1);
  for (auto &voice : voices_) {
    if (voice.active && !voice.releasing && voice.bank_index == bank &&
        voice.slice_idx == sidx) {
      voice.releasing = true;
      voice_generation_.fetch_add(1, std::memory_order_relaxed);
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
  if (bank >= bank_count) return false;

  std::shared_ptr<const std::vector<float>> sample{};
  {
    const auto lock = std::scoped_lock{mutex_};
    sample = banks_[bank];
  }
  if (!sample || sample->empty()) return false;

  const auto ch = std::max<std::size_t>(bank_channels_[bank].load(), 1U);
  const auto frames = sample->size() / ch;
  const auto start_frame = static_cast<std::size_t>(
      bank_trim_start_[bank].load() * static_cast<float>(frames));
  const auto end_frame = std::min(
      static_cast<std::size_t>(bank_trim_end_[bank].load() * static_cast<float>(frames)),
      frames);

  if (start_frame >= end_frame) return false;

  const auto begin = sample->cbegin() + static_cast<std::ptrdiff_t>(start_frame * ch);
  const auto end_it = sample->cbegin() + static_cast<std::ptrdiff_t>(end_frame * ch);
  return write_wav_f32(path, std::vector<float>(begin, end_it),
                       static_cast<std::uint32_t>(ch), sample_rate);
}

bool AudioSampler::has_sample() const {
  const auto bank = active_bank_.load();
  if (!bank_wav_file_[bank].empty()) return true;
  const auto lock = std::scoped_lock{mutex_};
  return banks_[bank] && !banks_[bank]->empty();
}

std::size_t AudioSampler::sample_frames() const {
  return bank_frames(active_bank_.load());
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
  return bank_waveform(active_bank_.load(), n_points);
}

std::vector<float> AudioSampler::bank_wavetable_cycle(std::size_t bank, std::size_t n_points) const {
  if (bank >= bank_count || n_points == 0 || !bank_is_wavetable_[bank].load()) {
    return std::vector<float>(n_points, 0.0F);
  }
  const auto lock = std::scoped_lock{mutex_};
  if (!banks_[bank] || banks_[bank]->empty()) {
    return std::vector<float>(n_points, 0.0F);
  }
  const auto &src = *banks_[bank];
  std::vector<float> result(n_points);
  for (std::size_t i = 0; i < n_points; ++i) {
    const auto idx = i * src.size() / n_points;
    result[i] = idx < src.size() ? src[idx] : 0.0F;
  }
  return result;
}

std::vector<float> AudioSampler::bank_waveform(std::size_t bank, std::size_t n_points) const {
  if (bank >= bank_count || n_points == 0) {
    return std::vector<float>(n_points, 0.0F);
  }
  const auto lock = std::scoped_lock{mutex_};
  const auto &cached = bank_cached_waveforms_[bank];
  if (!cached || cached->empty()) {
    return std::vector<float>(n_points, 0.0F);
  }
  if (cached->size() == n_points) return *cached;
  // Resample the cached waveform to the requested point count.
  std::vector<float> result(n_points, 0.0F);
  for (std::size_t i = 0; i < n_points; ++i) {
    const auto src = i * (cached->size() - 1U) / (n_points - 1U);
    result[i] = (*cached)[src];
  }
  return result;
}

AudioSampler::BankSnapshot AudioSampler::bank_snapshot(std::size_t bank) const {
  if (bank >= bank_count || bank_is_wavetable_[bank].load() ||
      bank_stream_[bank].load()) {
    return {};
  }
  const auto lock = std::scoped_lock{mutex_};
  return BankSnapshot{
      .samples = banks_[bank],
      .channel_count = bank_channels_[bank].load(),
      .trim_start = bank_trim_start_[bank].load(),
      .trim_end = bank_trim_end_[bank].load(),
  };
}

bool AudioSampler::bank_has_sample(std::size_t bank) const {
  if (bank >= bank_count) return false;
  if (!bank_wav_file_[bank].empty()) return true;
  const auto lock = std::scoped_lock{mutex_};
  return banks_[bank] && !banks_[bank]->empty();
}

std::size_t AudioSampler::bank_frames(std::size_t bank) const {
  if (bank >= bank_count) return 0U;
  if (bank_file_backed_[bank].load()) {
    return static_cast<std::size_t>(bank_file_frames_[bank].load());
  }
  const auto lock = std::scoped_lock{mutex_};
  const auto channel_count = static_cast<std::size_t>(bank_channels_[bank].load());
  return banks_[bank] && channel_count > 0U ? banks_[bank]->size() / channel_count : 0U;
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

bool AudioSampler::bank_is_long_sample(std::size_t bank) const {
  if (bank >= bank_count || bank_is_wavetable_[bank].load()) {
    return false;
  }
  const auto lock = std::scoped_lock{mutex_};
  if (!banks_[bank]) {
    return false;
  }
  const auto channel_count = std::max<std::size_t>(bank_channels_[bank].load(), 1U);
  return banks_[bank]->size() / channel_count > static_cast<std::size_t>(sample_rate) * 2U;
}

bool AudioSampler::bank_is_stream(std::size_t bank) const {
  if (bank >= bank_count) {
    return false;
  }
  return bank_stream_[bank].load();
}

bool AudioSampler::is_running() const { return running_; }

std::uint32_t AudioSampler::bank_waveform_version(std::size_t bank) const {
  if (bank >= bank_count) return 0U;
  return bank_waveform_ver_[bank].load(std::memory_order_relaxed);
}

std::uint32_t AudioSampler::stem_waveform_version(std::size_t stem) const {
  if (stem >= stem_count) return 0U;
  return stem_waveform_ver_[stem].load(std::memory_order_relaxed);
}

AudioSampler::PerfSnapshot AudioSampler::perf_snapshot() const {
  PerfSnapshot s{};
  s.callback_count = perf_.callback_count.load(std::memory_order_relaxed);
  const auto ns_sum = perf_.ns_sum.load(std::memory_order_relaxed);
  s.avg_callback_us = s.callback_count > 0U
      ? static_cast<double>(ns_sum) / static_cast<double>(s.callback_count) / 1000.0
      : 0.0;
  s.max_callback_us =
      static_cast<double>(perf_.ns_max.load(std::memory_order_relaxed)) / 1000.0;
  s.voice_steals = perf_.voice_steals.load(std::memory_order_relaxed);
  s.peak_voices = perf_.peak_voices.load(std::memory_order_relaxed);
  return s;
}

void AudioSampler::perf_reset() {
  perf_.callback_count.store(0U, std::memory_order_relaxed);
  perf_.ns_sum.store(0U, std::memory_order_relaxed);
  perf_.ns_max.store(0U, std::memory_order_relaxed);
  perf_.voice_steals.store(0U, std::memory_order_relaxed);
  perf_.peak_voices.store(0U, std::memory_order_relaxed);
}

void AudioSampler::stream_play(std::size_t bank, float rate) {
  if (bank >= bank_count) return;
  scrub_active_[bank].store(false, std::memory_order_relaxed);
  const auto ts = bank_trim_start_[bank].load();
  const auto te = bank_trim_end_[bank].load();
  bank_playback_rate_[bank].store(rate, std::memory_order_relaxed);
  bank_stream_start_frac_[bank].store(ts, std::memory_order_relaxed);
  bank_stream_end_frac_[bank].store(te, std::memory_order_relaxed);
  if (bank_file_backed_[bank].load()) {
    wav_streams_[bank]->set_loop(bank_stream_loop_[bank].load());
    wav_streams_[bank]->start(bank_wav_file_[bank], ts, te);
    return;
  }
  std::uint64_t start_pos = 0;
  {
    const auto lock = std::scoped_lock{mutex_};
    if (!banks_[bank] || banks_[bank]->empty()) return;
    const auto ch = std::max<std::uint64_t>(bank_channels_[bank].load(), 1U);
    const auto frames = static_cast<std::uint64_t>(banks_[bank]->size() / ch);
    start_pos = static_cast<std::uint64_t>(ts * static_cast<float>(frames));
    start_pos = std::min(start_pos, frames > 0 ? frames - 1 : 0);
  }
  stream_active_[bank].store(false, std::memory_order_release);
  stream_frac_pos_[bank].store(0.0F, std::memory_order_relaxed);
  stream_pos_[bank].store(start_pos, std::memory_order_relaxed);
  stream_active_[bank].store(true, std::memory_order_release);
}

void AudioSampler::stream_play_slice(std::size_t bank, int slice_idx) {
  if (bank >= bank_count) return;
  scrub_active_[bank].store(false, std::memory_order_relaxed);
  const auto n = bank_slice_count_[bank].load();
  if (n <= 0) { stream_play(bank, 1.0F); return; }
  const auto sidx = std::clamp(slice_idx, 0, n - 1);
  const auto ts   = bank_trim_start_[bank].load();
  const auto te   = bank_trim_end_[bank].load();
  const auto range      = te - ts;
  const auto slice_len  = range / static_cast<float>(n);
  const auto start_frac = ts + static_cast<float>(sidx) * slice_len;
  const auto end_frac   = start_frac + slice_len;

  bank_playback_rate_[bank].store(1.0F, std::memory_order_relaxed);
  bank_stream_start_frac_[bank].store(start_frac, std::memory_order_relaxed);
  bank_stream_end_frac_[bank].store(end_frac, std::memory_order_relaxed);

  if (bank_file_backed_[bank].load()) {
    wav_streams_[bank]->set_loop(bank_stream_loop_[bank].load());
    wav_streams_[bank]->start(bank_wav_file_[bank], start_frac, end_frac);
    return;
  }
  std::uint64_t start_pos = 0;
  {
    const auto lock = std::scoped_lock{mutex_};
    if (!banks_[bank] || banks_[bank]->empty()) return;
    const auto ch     = std::max<std::uint64_t>(bank_channels_[bank].load(), 1U);
    const auto frames = static_cast<std::uint64_t>(banks_[bank]->size() / ch);
    start_pos = static_cast<std::uint64_t>(start_frac * static_cast<float>(frames));
    start_pos = std::min(start_pos, frames > 0 ? frames - 1 : 0);
  }
  stream_active_[bank].store(false, std::memory_order_release);
  stream_frac_pos_[bank].store(0.0F, std::memory_order_relaxed);
  stream_pos_[bank].store(start_pos, std::memory_order_relaxed);
  stream_active_[bank].store(true, std::memory_order_release);
}

void AudioSampler::stream_seek(std::size_t bank, float frac) {
  if (bank >= bank_count) return;
  const auto cf = std::clamp(frac, 0.0F, 1.0F);
  if (bank_file_backed_[bank].load()) {
    wav_streams_[bank]->seek(cf);
    return;
  }
  const auto lock = std::scoped_lock{mutex_};
  if (!banks_[bank] || banks_[bank]->empty()) return;
  const auto ch     = std::max<std::uint64_t>(bank_channels_[bank].load(), 1U);
  const auto frames = banks_[bank]->size() / ch;
  const auto sf     = bank_stream_start_frac_[bank].load();
  const auto ef     = bank_stream_end_frac_[bank].load();
  const auto range  = static_cast<double>(ef - sf) * static_cast<double>(frames);
  const auto target = static_cast<std::uint64_t>(
      static_cast<double>(sf) * static_cast<double>(frames) + static_cast<double>(cf) * range);
  stream_frac_pos_[bank].store(0.0F, std::memory_order_relaxed);
  stream_pos_[bank].store(std::min(target, frames > 0 ? frames - 1 : 0),
                          std::memory_order_relaxed);
}

void AudioSampler::set_bank_stream_loop(std::size_t bank, bool loop) {
  if (bank >= bank_count) return;
  bank_stream_loop_[bank].store(loop, std::memory_order_release);
  if (bank_file_backed_[bank].load()) {
    wav_streams_[bank]->set_loop(loop);
  }
}

bool AudioSampler::bank_stream_loop(std::size_t bank) const {
  if (bank >= bank_count) return false;
  return bank_stream_loop_[bank].load(std::memory_order_relaxed);
}

void AudioSampler::begin_scrub(std::size_t bank, float initial_x) {
  if (bank >= bank_count) return;
  const auto x = std::clamp(initial_x, 0.0F, 1.0F);
  scrub_x_[bank].store(x, std::memory_order_relaxed);
  scrub_vel_[bank].store(0.0F, std::memory_order_relaxed);
  scrub_pos_init_[bank].store(true, std::memory_order_release);
  if (bank_file_backed_[bank].load()) {
    const auto ts = bank_trim_start_[bank].load();
    const auto te = bank_trim_end_[bank].load();
    wav_streams_[bank]->set_loop(bank_stream_loop_[bank].load());
    wav_streams_[bank]->start(bank_wav_file_[bank], ts, te);
    wav_streams_[bank]->seek(x);
  } else {
    stream_active_[bank].store(true, std::memory_order_release);
  }
  scrub_active_[bank].store(true, std::memory_order_release);
}

void AudioSampler::end_scrub(std::size_t bank) {
  if (bank >= bank_count) return;
  scrub_vel_[bank].store(0.0F, std::memory_order_relaxed);
  scrub_active_[bank].store(false, std::memory_order_release);
  stream_active_[bank].store(false, std::memory_order_relaxed);
}

void AudioSampler::set_scrub_x(std::size_t bank, float x) {
  if (bank >= bank_count) return;
  scrub_x_[bank].store(std::clamp(x, 0.0F, 1.0F), std::memory_order_relaxed);
}

void AudioSampler::set_scrub_velocity(std::size_t bank, float rate) {
  if (bank >= bank_count) return;
  constexpr float max_rate = 5.0F;
  scrub_vel_[bank].store(std::clamp(rate, -max_rate, max_rate),
                         std::memory_order_relaxed);
}

int AudioSampler::get_slice_page(std::size_t bank) const {
  if (bank >= bank_count) return 0;
  return slice_page_[bank];
}

void AudioSampler::set_slice_page(std::size_t bank, int page) {
  if (bank >= bank_count) return;
  slice_page_[bank] = std::max(0, page);
}

void AudioSampler::stream_stop(std::size_t bank) {
  if (bank >= bank_count) return;
  if (bank_file_backed_[bank].load()) {
    wav_streams_[bank]->stop();
    return;
  }
  stream_active_[bank].store(false, std::memory_order_release);
}

void AudioSampler::stream_stop_all() {
  for (std::size_t b = 0; b < bank_count; ++b) {
    if (bank_file_backed_[b].load()) {
      wav_streams_[b]->stop();
    }
    stream_active_[b].store(false, std::memory_order_release);
  }
}

bool AudioSampler::stream_is_active(std::size_t bank) const {
  if (bank >= bank_count) return false;
  if (bank_file_backed_[bank].load()) {
    return wav_streams_[bank]->is_active();
  }
  return stream_active_[bank].load(std::memory_order_acquire);
}

void AudioSampler::set_stem(std::size_t idx, std::string name, std::string path) {
  if (idx >= stem_count) return;
  stem_streams_[idx]->stop();
  {
    const auto lock = std::scoped_lock{mutex_};
    stem_names_[idx] = std::move(name);
    stem_paths_[idx] = std::move(path);
    stem_cached_waveforms_[idx].reset();
  }
  auto cfg = ma_decoder_config_init(ma_format_f32, 2U, 48000U);
  ma_decoder dec{};
  ma_uint64 total_frames = 0;
  if (ma_decoder_init_file(stem_paths_[idx].c_str(), &cfg, &dec) == MA_SUCCESS) {
    ma_decoder_get_length_in_pcm_frames(&dec, &total_frames);
    ma_decoder_uninit(&dec);
  }
  stem_frames_[idx].store(total_frames);
  const auto new_count = std::max(loaded_stem_count_.load(), idx + 1);
  loaded_stem_count_.store(new_count);

  stem_waveform_ver_[idx].fetch_add(1U, std::memory_order_relaxed);
  const auto bpath = stem_paths_[idx];
  std::thread([this, idx, bpath] {
    auto wf = compute_file_waveform(bpath, 256U);
    const auto lock = std::scoped_lock{mutex_};
    stem_cached_waveforms_[idx] =
        std::make_shared<const std::vector<float>>(std::move(wf));
    stem_waveform_ver_[idx].fetch_add(1U, std::memory_order_relaxed);
  }).detach();
}

void AudioSampler::clear_stems() {
  for (std::size_t i = 0; i < stem_count; ++i)
    stem_streams_[i]->stop();
  {
    const auto lock = std::scoped_lock{mutex_};
    for (std::size_t i = 0; i < stem_count; ++i) {
      stem_names_[i].clear();
      stem_paths_[i].clear();
      stem_cached_waveforms_[i].reset();
      stem_frames_[i].store(0);
    }
  }
  loaded_stem_count_.store(0);
  active_stem_.store(0);
  for (auto &v : stem_waveform_ver_) {
    v.fetch_add(1U, std::memory_order_relaxed);
  }
}

void AudioSampler::stem_play(std::size_t idx) {
  if (idx >= stem_count || idx >= loaded_stem_count_.load()) return;
  std::string path;
  {
    const auto lock = std::scoped_lock{mutex_};
    path = stem_paths_[idx];
  }
  if (path.empty()) return;
  stem_streams_[idx]->start(path, 0.0F, 1.0F);
}

void AudioSampler::stem_stop(std::size_t idx) {
  if (idx >= stem_count) return;
  stem_streams_[idx]->stop();
}

void AudioSampler::stem_stop_all() {
  for (std::size_t i = 0; i < stem_count; ++i) {
    stem_streams_[i]->stop();
  }
}

bool AudioSampler::stem_is_active(std::size_t idx) const {
  if (idx >= stem_count) return false;
  return stem_streams_[idx]->is_active();
}

std::string AudioSampler::stem_name(std::size_t idx) const {
  if (idx >= stem_count) return {};
  const auto lock = std::scoped_lock{mutex_};
  return stem_names_[idx];
}

std::vector<float> AudioSampler::stem_waveform(std::size_t idx, std::size_t n_points) const {
  if (idx >= stem_count || n_points == 0) return std::vector<float>(n_points, 0.0F);
  const auto lock = std::scoped_lock{mutex_};
  const auto &cached = stem_cached_waveforms_[idx];
  if (!cached || cached->empty()) return std::vector<float>(n_points, 0.0F);
  if (cached->size() == n_points) return *cached;
  std::vector<float> result(n_points, 0.0F);
  for (std::size_t i = 0; i < n_points; ++i) {
    const auto src = i * (cached->size() - 1U) / (n_points - 1U);
    result[i] = (*cached)[src];
  }
  return result;
}

std::uint64_t AudioSampler::stem_frame_count(std::size_t idx) const {
  if (idx >= stem_count) return 0;
  return stem_frames_[idx].load();
}

std::size_t AudioSampler::loaded_stem_count() const {
  return loaded_stem_count_.load();
}

void AudioSampler::set_active_stem(std::size_t idx) {
  if (idx < stem_count) active_stem_.store(idx);
}

std::size_t AudioSampler::active_stem() const {
  return active_stem_.load();
}

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

  const auto _cb_t0 = std::chrono::steady_clock::now();

  std::array<std::shared_ptr<const std::vector<float>>, bank_count> banks{};
  std::array<std::shared_ptr<const std::vector<float>>, bank_count> bank_morph_banks{};
  std::array<std::uint32_t, bank_count> bank_channels{};
  std::array<std::uint64_t, bank_count> stream_positions{};
  std::array<std::uint64_t, bank_count> initial_stream_positions{};
  std::array<bool, bank_count> stream_active{};
  std::array<bool, bank_count> initial_stream_active{};
  std::array<float, bank_count> bank_playback_rates{};
  std::array<float, bank_count> stream_frac_pos{};
  std::array<float, bank_count> initial_stream_frac_pos{};
  std::array<float, bank_count> bank_stream_start_fracs{};
  std::array<float, bank_count> bank_stream_end_fracs{};
  std::array<bool,  bank_count> bank_stream_loops{};
  std::array<float, bank_count> scrub_xs{};
  std::array<float, bank_count> scrub_vels{};
  std::array<bool,  bank_count> scrub_actives{};
  std::array<Voice, voice_count> voices{};
  std::uint64_t voice_generation{};

  {
    const auto lock = std::scoped_lock{self->mutex_};
    banks = self->banks_;
    bank_morph_banks = self->bank_morph_banks_;
    for (std::size_t b = 0; b < bank_count; ++b) {
      bank_channels[b] = self->bank_channels_[b].load();
      stream_positions[b] = self->stream_pos_[b].load(std::memory_order_relaxed);
      initial_stream_positions[b] = stream_positions[b];
      stream_active[b] = self->stream_active_[b].load(std::memory_order_relaxed);
      initial_stream_active[b] = stream_active[b];
      bank_playback_rates[b]     = self->bank_playback_rate_[b].load(std::memory_order_relaxed);
      stream_frac_pos[b]         = self->stream_frac_pos_[b].load(std::memory_order_relaxed);
      initial_stream_frac_pos[b] = stream_frac_pos[b];
      bank_stream_start_fracs[b] = self->bank_stream_start_frac_[b].load(std::memory_order_relaxed);
      bank_stream_end_fracs[b]   = self->bank_stream_end_frac_[b].load(std::memory_order_relaxed);
      bank_stream_loops[b]       = self->bank_stream_loop_[b].load(std::memory_order_relaxed);
      scrub_xs[b]      = self->scrub_x_[b].load(std::memory_order_relaxed);
      scrub_vels[b]    = self->scrub_vel_[b].load(std::memory_order_relaxed);
      scrub_actives[b] = self->scrub_active_[b].load(std::memory_order_acquire);
    }
    voices = self->voices_;
    voice_generation =
        self->voice_generation_.load(std::memory_order_relaxed);
  }

  // Pre-compute trim boundaries (in samples) for each bank
  std::array<double, bank_count> trim_starts{};
  std::array<double, bank_count> trim_ends{};
  for (std::size_t b = 0; b < bank_count; ++b) {
    const auto channel_count = std::max(bank_channels[b], 1U);
    const auto sz = banks[b] ? static_cast<double>(banks[b]->size() / channel_count) : 0.0;
    trim_starts[b] = static_cast<double>(self->bank_trim_start_[b].load()) * sz;
    trim_ends[b] = static_cast<double>(self->bank_trim_end_[b].load()) * sz;
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

  const auto unison_depth = std::sqrt(wavetable_unison);
  const auto has_unison = unison_depth > 0.001F;
  const auto spread_d = has_unison
      ? static_cast<double>(std::pow(2.0F, (72.0F * unison_depth) / 1200.0F))
      : 1.0;
  const auto has_vibrato = vibrato_depth > 0.001F;
  const auto static_pitch_rate = has_vibrato
      ? 1.0F
      : std::pow(2.0F, pitch_bend * pitch_bend_range_semitones / 12.0F);

  for (ma_uint32 frame = 0; frame < frame_count; ++frame) {
    auto mixed_left = 0.0F;
    auto mixed_right = 0.0F;
    float pitch_rate;
    if (has_vibrato) {
      const auto vibrato =
          std::sin(self->vibrato_phase_) * vibrato_depth * vibrato_range_semitones;
      pitch_rate = std::pow(2.0F, (pitch_bend * pitch_bend_range_semitones + vibrato) / 12.0F);
    } else {
      pitch_rate = static_pitch_rate;
    }

    for (auto &voice : voices) {
      if (!voice.active) {
        continue;
      }

      const auto &sample = banks[voice.bank_index];
      if (!sample || sample->empty()) {
        voice.active = false;
        continue;
      }

      const auto trim_end = voice.trim_end_override >= 0.0
                                ? voice.trim_end_override
                                : trim_ends[voice.bank_index];
      const auto index = static_cast<std::size_t>(voice.position);

      const auto channel_count = std::max(bank_channels[voice.bank_index], 1U);
      const auto sample_frames = sample->size() / channel_count;

      if (index >= sample_frames || voice.position >= trim_end) {
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

      auto voice_left = voice.loop ? sample_at_loop(*sample, voice.position, channel_count, 0U)
                                   : sample_at(*sample, voice.position, channel_count, 0U);
      auto voice_right = channel_count > 1U
                             ? (voice.loop ? sample_at_loop(*sample, voice.position, channel_count, 1U)
                                           : sample_at(*sample, voice.position, channel_count, 1U))
                             : voice_left;
      auto voice_sample = (voice_left + voice_right) * 0.5F;
      const auto &morph_sample = bank_morph_banks[voice.bank_index];
      const auto has_morph = voice.loop && morph_sample && !morph_sample->empty();

      if (has_morph) {
        const auto target = sample_at_loop(*morph_sample, voice.position, 1U, 0U);
        voice_sample = voice_sample + (target - voice_sample) * wavetable_morph;
        voice_left = voice_sample;
        voice_right = voice_sample;
      }

      if (has_unison) {
        auto lower = voice.loop
                         ? sample_at_loop(*sample, voice.position / spread_d, channel_count, 0U)
                         : sample_at(*sample, voice.position / spread_d, channel_count, 0U);
        auto upper = voice.loop
                         ? sample_at_loop(*sample, voice.position * spread_d, channel_count, 0U)
                         : sample_at(*sample, voice.position * spread_d, channel_count, 0U);
        if (has_morph) {
          const auto lower_target =
              sample_at_loop(*morph_sample, voice.position / spread_d, 1U, 0U);
          const auto upper_target =
              sample_at_loop(*morph_sample, voice.position * spread_d, 1U, 0U);
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
          voice.position += voice.rate * static_cast<double>(pitch_rate);
        }
      } else {
        voice.position += voice.rate * static_cast<double>(pitch_rate);
      }

      const auto vl = (voice.pan <= 0.0F ? 1.0F : 1.0F - voice.pan) * voice.velocity_gain;
      const auto vr = (voice.pan >= 0.0F ? 1.0F : 1.0F + voice.pan) * voice.velocity_gain;
      mixed_left  += voice_left  * voice.envelope * gain * vl;
      mixed_right += voice_right * voice.envelope * gain * vr;
    }

    self->vibrato_phase_ +=
        2.0F * pi * vibrato_frequency / static_cast<float>(sample_rate);

    if (self->vibrato_phase_ >= 2.0F * pi) {
      self->vibrato_phase_ -= 2.0F * pi;
    }

    const auto tremolo = has_vibrato
        ? 1.0F - vibrato_depth * 0.35F * (1.0F - std::sin(self->vibrato_phase_)) * 0.5F
        : 1.0F;

    constexpr auto stream_gain_scale = 0.8F;
    const auto n_stems = self->loaded_stem_count_.load(std::memory_order_relaxed);
    for (std::size_t s = 0; s < n_stems; ++s) {
      float l = 0.0F;
      float r = 0.0F;
      if (self->stem_streams_[s]->read_frame(l, r)) {
        mixed_left += l * gain * stream_gain_scale * tremolo;
        mixed_right += r * gain * stream_gain_scale * tremolo;
      }
    }
    for (std::size_t b = 0; b < bank_count; ++b) {
      if (self->bank_file_backed_[b].load(std::memory_order_relaxed)) {
        float l = 0.0F;
        float r = 0.0F;
        if (scrub_actives[b]) {
          if (self->scrub_pos_init_[b].load(std::memory_order_relaxed)) {
            self->scrub_pos_init_[b].store(false, std::memory_order_relaxed);
            self->scrub_pos_cb_[b] = static_cast<double>(scrub_xs[b]);
            self->scrub_vel_cb_[b] = 0.0;
          }
          const double target_rate = static_cast<double>(scrub_vels[b]);
          self->scrub_vel_cb_[b] += (target_rate - self->scrub_vel_cb_[b]) * 0.045;
          const auto rate = static_cast<float>(self->scrub_vel_cb_[b]);
          if (rate > 0.01F && self->wav_streams_[b]->read_frame(l, r, rate)) {
            mixed_left  += l * gain * stream_gain_scale * tremolo;
            mixed_right += r * gain * stream_gain_scale * tremolo;
          }
        } else {
          self->scrub_pos_cb_[b] = 0.0;
          self->scrub_vel_cb_[b] = 0.0;
          if (self->wav_streams_[b]->read_frame(l, r, bank_playback_rates[b] * pitch_rate)) {
            mixed_left  += l * gain * stream_gain_scale * tremolo;
            mixed_right += r * gain * stream_gain_scale * tremolo;
          }
        }
        continue;
      }
      if (!stream_active[b] && !scrub_actives[b]) continue;
      const auto &sample = banks[b];
      if (!sample || sample->empty()) {
        stream_active[b] = false;
        continue;
      }
      const auto ch            = std::max(bank_channels[b], 1U);
      const auto stream_frames = sample->size() / ch;
      const auto start_fr      = static_cast<std::uint64_t>(
          bank_stream_start_fracs[b] * static_cast<float>(stream_frames));
      const auto end_frame     = std::min(
          static_cast<std::uint64_t>(bank_stream_end_fracs[b] * static_cast<float>(stream_frames)),
          static_cast<std::uint64_t>(stream_frames));

      if (scrub_actives[b]) {
        const auto x = static_cast<double>(scrub_xs[b]);
        const auto window = static_cast<double>(end_frame > start_fr ? end_frame - start_fr : 1U);
        if (self->scrub_pos_init_[b].load(std::memory_order_relaxed)) {
          self->scrub_pos_cb_[b] = static_cast<double>(start_fr) + x * window;
          self->scrub_vel_cb_[b] = 0.0;
          self->scrub_pos_init_[b].store(false, std::memory_order_relaxed);
        }
        const double target_rate = static_cast<double>(scrub_vels[b]);
        self->scrub_vel_cb_[b] += (target_rate - self->scrub_vel_cb_[b]) * 0.045;

        if (std::abs(self->scrub_vel_cb_[b]) < 0.001) continue;

        const auto clamped = std::clamp(
            self->scrub_pos_cb_[b] + self->scrub_vel_cb_[b],
            static_cast<double>(start_fr),
            static_cast<double>(end_frame > 0U ? end_frame - 1U : 0U));
        self->scrub_pos_cb_[b] = clamped;

        const auto pos0 = static_cast<std::uint64_t>(clamped);
        const auto pos1 = std::min(pos0 + 1U, end_frame > 0U ? end_frame - 1U : 0U);
        const auto frac  = static_cast<float>(clamped - static_cast<double>(pos0));
        const auto sl    = (*sample)[pos0 * ch];
        const auto sl1   = (*sample)[pos1 * ch];
        const auto left  = sl  + (sl1  - sl)  * frac;
        const auto sr    = ch > 1U ? (*sample)[pos0 * ch + 1U] : sl;
        const auto sr1   = ch > 1U ? (*sample)[pos1 * ch + 1U] : sl1;
        const auto right = sr  + (sr1  - sr)  * frac;
        mixed_left  += left  * gain * stream_gain_scale * tremolo;
        mixed_right += right * gain * stream_gain_scale * tremolo;
        stream_positions[b] = pos0;
        stream_frac_pos[b]  = frac;
      } else {
        self->scrub_pos_cb_[b] = 0.0;

        const auto eff_rate_d = static_cast<double>(bank_playback_rates[b] * pitch_rate);

        if (stream_positions[b] >= end_frame) {
          if (bank_stream_loops[b]) {
            stream_positions[b] = start_fr;
            stream_frac_pos[b]  = 0.0F;
          } else {
            stream_active[b] = false;
            continue;
          }
        }

        const auto pos0 = stream_positions[b];
        const auto pos1 = std::min(pos0 + 1U, end_frame > 0U ? end_frame - 1U : 0U);
        const auto frac  = stream_frac_pos[b];
        const auto sl    = (*sample)[pos0 * ch];
        const auto sl1   = (*sample)[pos1 * ch];
        const auto left  = sl  + (sl1  - sl)  * frac;
        const auto sr    = ch > 1U ? (*sample)[pos0 * ch + 1U] : sl;
        const auto sr1   = ch > 1U ? (*sample)[pos1 * ch + 1U] : sl1;
        const auto right = sr  + (sr1  - sr)  * frac;
        mixed_left  += left  * gain * stream_gain_scale * tremolo;
        mixed_right += right * gain * stream_gain_scale * tremolo;

        stream_frac_pos[b] += static_cast<float>(eff_rate_d);
        const auto step = static_cast<std::uint64_t>(stream_frac_pos[b]);
        stream_frac_pos[b] -= static_cast<float>(step);
        stream_positions[b] += step;
      }
    }

    const auto peak = std::max(std::abs(mixed_left), std::abs(mixed_right));
    if (peak > 1.0F) {
      mixed_left /= peak;
      mixed_right /= peak;
    }

    const auto output_index = static_cast<std::size_t>(frame) * channels;
    out[output_index] = std::clamp(mixed_left, -1.0F, 1.0F);
    out[output_index + 1] = std::clamp(mixed_right, -1.0F, 1.0F);
  }

  {
    const auto lock = std::scoped_lock{self->mutex_};
    if (self->voice_generation_.load(std::memory_order_relaxed) ==
        voice_generation) {
      self->voices_ = voices;
    }
    for (std::size_t b = 0; b < bank_count; ++b) {
      const auto current_active =
          self->stream_active_[b].load(std::memory_order_relaxed);
      const auto current_pos =
          self->stream_pos_[b].load(std::memory_order_relaxed);
      if (current_active == initial_stream_active[b] &&
          current_pos == initial_stream_positions[b]) {
        self->stream_active_[b].store(stream_active[b], std::memory_order_relaxed);
        self->stream_pos_[b].store(stream_positions[b], std::memory_order_relaxed);
        self->stream_frac_pos_[b].store(stream_frac_pos[b], std::memory_order_relaxed);
      } else if (initial_stream_active[b] && !stream_active[b] &&
                 current_active == initial_stream_active[b]) {
        self->stream_active_[b].store(false, std::memory_order_relaxed);
        self->stream_pos_[b].store(stream_positions[b], std::memory_order_relaxed);
        self->stream_frac_pos_[b].store(stream_frac_pos[b], std::memory_order_relaxed);
      }
    }
  }

  // Perf counters
  {
    const auto cb_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - _cb_t0).count());
    self->perf_.callback_count.fetch_add(1U, std::memory_order_relaxed);
    self->perf_.ns_sum.fetch_add(cb_ns, std::memory_order_relaxed);
    auto cur_max = self->perf_.ns_max.load(std::memory_order_relaxed);
    while (cb_ns > cur_max &&
           !self->perf_.ns_max.compare_exchange_weak(
               cur_max, cb_ns, std::memory_order_relaxed)) {}

    std::uint32_t n_active = 0U;
    for (const auto& v : voices) n_active += v.active ? 1U : 0U;
    auto cur_peak = self->perf_.peak_voices.load(std::memory_order_relaxed);
    while (n_active > cur_peak &&
           !self->perf_.peak_voices.compare_exchange_weak(
               cur_peak, n_active, std::memory_order_relaxed)) {}
  }
}

float AudioSampler::sample_at(const std::vector<float> &sample,
                              double position,
                              std::uint32_t channel_count,
                              std::uint32_t channel) {
  const auto sample_channels = std::max<std::size_t>(channel_count, 1U);
  if (sample.empty() || channel >= sample_channels) {
    return 0.0F;
  }

  const auto lower_index = static_cast<std::size_t>(position);
  const auto frame_count = sample.size() / sample_channels;

  if (lower_index >= frame_count) {
    return 0.0F;
  }

  const auto upper_index = std::min(lower_index + 1, frame_count - 1);
  const auto fraction = static_cast<float>(position - static_cast<double>(lower_index));
  const auto lower = sample[lower_index * sample_channels + channel];
  const auto upper = sample[upper_index * sample_channels + channel];

  return lower + (upper - lower) * fraction;
}

float AudioSampler::sample_at_loop(const std::vector<float> &sample,
                                   double position,
                                   std::uint32_t channel_count,
                                   std::uint32_t channel) {
  const auto sample_channels = std::max<std::size_t>(channel_count, 1U);
  const auto frame_count = sample.size() / sample_channels;
  if (sample.empty() || frame_count == 0U || channel >= sample_channels) {
    return 0.0F;
  }

  const auto size = static_cast<double>(frame_count);
  auto wrapped = position;
  if (wrapped >= size) wrapped -= size;
  if (wrapped < 0.0) wrapped += size;

  const auto lower_index = static_cast<std::size_t>(wrapped);
  const auto upper_index = (lower_index + 1U) % frame_count;
  const auto fraction = static_cast<float>(wrapped - static_cast<double>(lower_index));
  const auto lower = sample[lower_index * sample_channels + channel];
  const auto upper = sample[upper_index * sample_channels + channel];

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

std::vector<float> AudioSampler::compute_waveform(const std::vector<float> &samples,
                                                   std::uint32_t channel_count,
                                                   std::size_t n_points) {
  const auto ch = std::max(channel_count, 1U);
  const auto frame_count = samples.size() / ch;
  if (frame_count == 0 || n_points == 0) return std::vector<float>(n_points, 0.0F);
  std::vector<float> result(n_points, 0.0F);
  for (std::size_t i = 0; i < n_points; ++i) {
    const auto f0 = (i * frame_count) / n_points;
    const auto f1 = std::max(((i + 1U) * frame_count) / n_points, f0 + 1U);
    float peak = 0.0F;
    for (auto f = f0; f < f1 && f < frame_count; ++f) {
      for (std::size_t c = 0; c < ch; ++c) {
        peak = std::max(peak, std::abs(samples[f * ch + c]));
      }
    }
    result[i] = std::isfinite(peak) ? std::clamp(peak, 0.0F, 1.0F) : 0.0F;
  }
  return result;
}

std::vector<float> AudioSampler::compute_file_waveform(const std::string &path,
                                                        std::size_t n_points) {
  if (n_points == 0) return {};
  auto cfg = ma_decoder_config_init(ma_format_f32, 2U, 48000U);
  ma_decoder dec{};
  if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) return {};

  ma_uint64 total_frames = 0;
  ma_decoder_get_length_in_pcm_frames(&dec, &total_frames);
  if (total_frames == 0) { ma_decoder_uninit(&dec); return {}; }

  std::vector<float> result(n_points, 0.0F);
  const auto frames_per_point = std::max<ma_uint64>(total_frames / static_cast<ma_uint64>(n_points), 1U);
  constexpr ma_uint64 read_frames = 512U;
  std::array<float, read_frames * 2U> buf{};

  for (std::size_t i = 0; i < n_points; ++i) {
    const auto seek_frame = static_cast<ma_uint64>(i) * frames_per_point;
    ma_decoder_seek_to_pcm_frame(&dec, seek_frame);
    ma_uint64 frames_read = 0;
    ma_decoder_read_pcm_frames(&dec, buf.data(),
                               std::min(frames_per_point, read_frames), &frames_read);
    float peak = 0.0F;
    for (ma_uint64 f = 0; f < frames_read; ++f) {
      peak = std::max(peak, std::abs(buf[f * 2U]));
      peak = std::max(peak, std::abs(buf[f * 2U + 1U]));
    }
    result[i] = std::clamp(peak, 0.0F, 1.0F);
  }
  ma_decoder_uninit(&dec);
  return result;
}

} // namespace analogno
