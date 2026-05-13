#pragma once

#include <miniaudio.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

namespace analogno {

class AudioSampler final {
public:
  static constexpr auto bank_count = std::size_t{8};

  AudioSampler();
  ~AudioSampler();

  AudioSampler(const AudioSampler &) = delete;
  AudioSampler &operator=(const AudioSampler &) = delete;

  AudioSampler(AudioSampler &&) = delete;
  AudioSampler &operator=(AudioSampler &&) = delete;

  // Operates on the active bank
  void set_sample(std::vector<float> sample);
  void set_wavetable(std::vector<float> samples); // looping single-cycle wavetable
  void clear_sample();
  void set_trim(float start, float end);
  void set_gain(float gain);
  void set_pitch_controls(float pitch_bend, float vibrato_depth);
  void trigger(float rate = 1.0F);
  void release(float rate = 1.0F); // begin release for matching active voice

  void set_active_bank(std::size_t bank);
  bool save_bank(std::size_t bank, const std::string &path);

  // Active-bank queries
  [[nodiscard]] bool has_sample() const;
  [[nodiscard]] std::size_t sample_frames() const;
  [[nodiscard]] float trim_start() const;
  [[nodiscard]] float trim_end() const;
  [[nodiscard]] std::size_t active_bank() const;

  // Per-bank queries
  [[nodiscard]] bool bank_has_sample(std::size_t bank) const;
  [[nodiscard]] std::size_t bank_frames(std::size_t bank) const;
  [[nodiscard]] float bank_trim_start(std::size_t bank) const;
  [[nodiscard]] float bank_trim_end(std::size_t bank) const;

  [[nodiscard]] bool is_running() const;

private:
  struct Voice final {
    bool active{};
    bool releasing{};
    bool loop{};       // wavetable: loops between trim_start and trim_end
    float position{};
    float rate{1.0F};
    float envelope{};
    std::size_t bank_index{};
  };

  static constexpr auto voice_count = std::size_t{8};
  static constexpr auto sample_rate = ma_uint32{48000};
  static constexpr auto channels = ma_uint32{2};
  static constexpr auto attack_frames = float{192.0F};
  static constexpr auto release_frames = float{960.0F};

  ma_device device_{};
  std::array<std::shared_ptr<const std::vector<float>>, bank_count> banks_{};
  std::array<Voice, voice_count> voices_{};
  mutable std::mutex mutex_{};
  std::array<std::atomic<float>, bank_count> bank_trim_start_{};
  std::array<std::atomic<float>, bank_count> bank_trim_end_{};
  std::array<std::atomic<bool>, bank_count> bank_is_wavetable_{};
  std::atomic<std::size_t> active_bank_{};
  std::atomic<float> gain_{};
  std::atomic<float> pitch_bend_{};
  std::atomic<float> vibrato_depth_{};
  float vibrato_phase_{};
  bool device_ready_{};
  bool running_{};
  std::size_t next_voice_{};

  static void playback_callback(ma_device *device, void *output,
                                const void *input, ma_uint32 frame_count);
  [[nodiscard]] static float sample_at(const std::vector<float> &sample,
                                       float position);
};

} // namespace analogno
