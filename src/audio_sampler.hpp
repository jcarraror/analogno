#pragma once

#include "wav_stream.hpp"

#include <miniaudio.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace analogno {

class AudioSampler final {
public:
  static constexpr auto bank_count  = std::size_t{8};
  static constexpr auto voice_count = std::size_t{32};
  static constexpr auto stem_count = std::size_t{8};

  explicit AudioSampler(ma_context *shared_ctx = nullptr);
  ~AudioSampler();

  AudioSampler(const AudioSampler &) = delete;
  AudioSampler &operator=(const AudioSampler &) = delete;

  AudioSampler(AudioSampler &&) = delete;
  AudioSampler &operator=(AudioSampler &&) = delete;

  // Operates on the active bank
  void set_sample(std::vector<float> sample);
  void set_sample(std::vector<float> sample, std::uint32_t channel_count,
                  std::uint32_t src_rate = 48000U);
  void set_sample_for_bank(std::size_t bank, std::vector<float> sample,
                           std::uint32_t channel_count, std::uint32_t src_rate = 48000U);
  void set_bank_file(std::size_t bank, std::string path);

  // Raw stream playback
  void stream_play(std::size_t bank, float rate = 1.0F);
  void stream_stop(std::size_t bank);
  void stream_stop_all();
  [[nodiscard]] bool stream_is_active(std::size_t bank) const;

  // Stem player
  void set_stem(std::size_t idx, std::string name, std::string path);
  void clear_stems();
  void stem_play(std::size_t idx);
  void stem_stop(std::size_t idx);
  void stem_stop_all();
  [[nodiscard]] bool stem_is_active(std::size_t idx) const;
  [[nodiscard]] std::string stem_name(std::size_t idx) const;
  [[nodiscard]] std::uint64_t stem_frame_count(std::size_t idx) const;
  [[nodiscard]] std::vector<float> stem_waveform(std::size_t idx, std::size_t n_points = 256) const;
  [[nodiscard]] std::size_t loaded_stem_count() const;
  void set_active_stem(std::size_t idx);
  [[nodiscard]] std::size_t active_stem() const;
  void set_wavetable(std::vector<float> samples,
                     std::vector<float> morph_samples = {}); // looping single-cycle wavetable
  void clear_sample();
  void set_trim(float start, float end);
  void set_bank_trim(std::size_t bank, float start, float end);
  void load_stem_to_bank(std::size_t bank, const std::string &path, float trim_start, float trim_end);
  void set_gain(float gain);
  void set_wavetable_controls(float morph, float noise, float unison);
  void set_pitch_controls(float pitch_bend, float vibrato_depth);
  void trigger(float rate = 1.0F, int midi_note = -1);
  void trigger_bank(std::size_t bank, float rate = 1.0F, int midi_note = -1, float velocity_gain = 1.0F, float pan = 0.0F);
  void trigger_bank_onset(std::size_t bank, int onset_idx, float velocity_gain = 1.0F, float pan = 0.0F);
  void release(float rate = 1.0F, int midi_note = -1);
  void release_bank(std::size_t bank, float rate = 1.0F, int midi_note = -1);
  void release_bank_onset(std::size_t bank, int onset_idx);
  void stop_all();

  void set_active_bank(std::size_t bank);
  bool save_bank(std::size_t bank, const std::string &path);

  // Active-bank queries
  [[nodiscard]] bool has_sample() const;
  [[nodiscard]] std::size_t sample_frames() const;
  [[nodiscard]] float trim_start() const;
  [[nodiscard]] float trim_end() const;
  [[nodiscard]] std::size_t active_bank() const;
  // Returns N peak-abs values downsampled from the active bank's sample.
  [[nodiscard]] std::vector<float> sample_waveform(std::size_t n_points = 256) const;
  [[nodiscard]] std::vector<float> bank_wavetable_cycle(std::size_t bank, std::size_t n_points = 128) const;

  void set_bank_root_note(std::size_t bank, int note);
  [[nodiscard]] int bank_root_note(std::size_t bank) const;
  void set_bank_slice_count(std::size_t bank, int count);
  [[nodiscard]] int bank_slice_count(std::size_t bank) const;
  void set_bank_onset_frames(std::size_t bank, std::vector<double> frame_positions);
  void clear_bank_onset_frames(std::size_t bank);
  [[nodiscard]] bool bank_has_onsets(std::size_t bank) const;

  struct BankSnapshot final {
    std::shared_ptr<const std::vector<float>> samples{};
    std::uint32_t channel_count{1};
    float trim_start{};
    float trim_end{1.0F};
  };
  [[nodiscard]] BankSnapshot bank_snapshot(std::size_t bank) const;

  // Per-bank queries
  [[nodiscard]] bool bank_has_sample(std::size_t bank) const;
  [[nodiscard]] std::size_t bank_frames(std::size_t bank) const;
  [[nodiscard]] float bank_trim_start(std::size_t bank) const;
  [[nodiscard]] float bank_trim_end(std::size_t bank) const;
  [[nodiscard]] bool bank_is_wavetable(std::size_t bank) const;
  [[nodiscard]] bool bank_is_long_sample(std::size_t bank) const;
  [[nodiscard]] bool bank_is_stream(std::size_t bank) const;
  [[nodiscard]] std::vector<float> bank_waveform(std::size_t bank, std::size_t n_points = 256) const;

  [[nodiscard]] bool is_running() const;

  [[nodiscard]] std::uint32_t bank_waveform_version(std::size_t bank) const;
  [[nodiscard]] std::uint32_t stem_waveform_version(std::size_t stem) const;

  struct PerfSnapshot final {
    std::uint64_t callback_count{};
    double avg_callback_us{};
    double max_callback_us{};
    std::uint64_t voice_steals{};
    std::uint32_t peak_voices{};
  };
  [[nodiscard]] PerfSnapshot perf_snapshot() const;
  void perf_reset();

private:
  struct Voice final {
    bool active{};
    bool releasing{};
    bool loop{};
    double position{};
    double rate{1.0};
    double trim_end_override{-1.0};
    int slice_idx{-1};
    float envelope{};
    float velocity_gain{1.0F};
    float pan{0.0F};
    std::size_t bank_index{};
    std::uint32_t noise_state{1};
  };

  static constexpr auto sample_rate = ma_uint32{48000};
  static constexpr auto channels = ma_uint32{2};
  static constexpr auto attack_frames = float{192.0F};
  static constexpr auto release_frames = float{960.0F};

  ma_device device_{};
  std::array<std::shared_ptr<const std::vector<float>>, bank_count> banks_{};
  std::array<std::shared_ptr<const std::vector<float>>, bank_count> bank_morph_banks_{};
  std::array<Voice, voice_count> voices_{};
  mutable std::mutex mutex_{};
  std::array<std::atomic<std::uint32_t>, bank_count> bank_channels_{};
  std::array<std::atomic<float>, bank_count> bank_trim_start_{};
  std::array<std::atomic<float>, bank_count> bank_trim_end_{};
  std::array<std::atomic<int>, bank_count> bank_root_note_{};
  std::array<std::atomic<int>, bank_count> bank_slice_count_{};
  std::array<std::vector<double>, bank_count> bank_onset_frames_{};
  std::array<std::atomic<bool>, bank_count> bank_is_wavetable_{};
  std::array<std::atomic<bool>, bank_count> bank_stream_{};
  std::array<std::atomic<bool>, bank_count> bank_file_backed_{};
  std::array<std::atomic<std::uint64_t>, bank_count> bank_file_frames_{};
  std::array<std::string, bank_count> bank_wav_file_{};
  std::array<std::unique_ptr<WavStream>, bank_count> wav_streams_{};
  std::array<std::shared_ptr<const std::vector<float>>, bank_count> bank_cached_waveforms_{};
  std::array<std::unique_ptr<WavStream>, stem_count> stem_streams_{};
  std::array<std::string, stem_count> stem_paths_{};
  std::array<std::string, stem_count> stem_names_{};
  std::array<std::atomic<std::uint64_t>, stem_count> stem_frames_{};
  std::array<std::shared_ptr<const std::vector<float>>, stem_count> stem_cached_waveforms_{};
  std::atomic<std::size_t> loaded_stem_count_{};
  std::atomic<std::size_t> active_stem_{};
  std::array<std::atomic<float>, bank_count> bank_playback_rate_{};
  std::atomic<std::size_t> active_bank_{};
  std::atomic<float> gain_{};
  std::atomic<float> wavetable_morph_{};
  std::atomic<float> wavetable_noise_{};
  std::atomic<float> wavetable_unison_{};
  std::atomic<float> pitch_bend_{};
  std::atomic<float> vibrato_depth_{};
  std::atomic<std::uint64_t> voice_generation_{};
  std::array<std::atomic<std::uint64_t>, bank_count> stream_pos_{};
  std::array<std::atomic<bool>, bank_count> stream_active_{};
  std::array<std::atomic<float>, bank_count> stream_frac_pos_{};
  float vibrato_phase_{};
  bool device_ready_{};
  bool running_{};
  std::size_t next_voice_{};

  std::array<std::atomic<std::uint32_t>, bank_count> bank_waveform_ver_{};
  std::array<std::atomic<std::uint32_t>, stem_count> stem_waveform_ver_{};

  struct PerfCounters final {
    std::atomic<std::uint64_t> callback_count{};
    std::atomic<std::uint64_t> ns_sum{};
    std::atomic<std::uint64_t> ns_max{};
    std::atomic<std::uint64_t> voice_steals{};
    std::atomic<std::uint32_t> peak_voices{};
  } perf_{};

  static void playback_callback(ma_device *device, void *output,
                                const void *input, ma_uint32 frame_count);
  [[nodiscard]] static float sample_at(const std::vector<float> &sample,
                                       double position,
                                       std::uint32_t channel_count,
                                       std::uint32_t channel);
  [[nodiscard]] static float sample_at_loop(const std::vector<float> &sample,
                                            double position,
                                            std::uint32_t channel_count,
                                            std::uint32_t channel);
  [[nodiscard]] static float noise_sample(std::uint32_t &state);
  [[nodiscard]] static std::vector<float>
  compute_waveform(const std::vector<float> &samples, std::uint32_t channel_count,
                   std::size_t n_points);
  [[nodiscard]] static std::vector<float>
  compute_file_waveform(const std::string &path, std::size_t n_points);
};

} // namespace analogno
