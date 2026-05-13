#pragma once

#include <miniaudio.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <memory>
#include <vector>

namespace analogno {

class AudioSampler final {
public:
  AudioSampler();
  ~AudioSampler();

  AudioSampler(const AudioSampler &) = delete;
  AudioSampler &operator=(const AudioSampler &) = delete;

  AudioSampler(AudioSampler &&) = delete;
  AudioSampler &operator=(AudioSampler &&) = delete;

  void set_sample(std::vector<float> sample);
  void clear_sample();
  void set_trim(float start, float end);
  void set_gain(float gain);
  void set_pitch_controls(float pitch_bend, float vibrato_depth);
  void trigger(float rate = 1.0F);

  [[nodiscard]] bool has_sample() const;
  [[nodiscard]] std::size_t sample_frames() const;
  [[nodiscard]] float trim_start() const;
  [[nodiscard]] float trim_end() const;
  [[nodiscard]] bool is_running() const;

private:
  struct Voice final {
    bool active{};
    bool releasing{};
    float position{};
    float rate{1.0F};
    float envelope{};
  };

  static constexpr auto voice_count = std::size_t{8};
  static constexpr auto sample_rate = ma_uint32{48000};
  static constexpr auto channels = ma_uint32{2};
  static constexpr auto attack_frames = float{192.0F};
  static constexpr auto release_frames = float{960.0F};

  ma_device device_{};
  std::shared_ptr<const std::vector<float>> sample_{};
  std::array<Voice, voice_count> voices_{};
  mutable std::mutex mutex_{};
  std::atomic<float> trim_start_{};
  std::atomic<float> trim_end_{1.0F};
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
