#pragma once

#include <miniaudio.h>

#include <array>
#include <cstddef>
#include <mutex>
#include <vector>

namespace analogno {

class AudioSampler final {
public:
  AudioSampler();
  ~AudioSampler();

  AudioSampler(const AudioSampler &) = delete;
  auto operator=(const AudioSampler &) -> AudioSampler & = delete;

  AudioSampler(AudioSampler &&) = delete;
  auto operator=(AudioSampler &&) -> AudioSampler & = delete;

  auto set_sample(std::vector<float> sample) -> void;
  auto clear_sample() -> void;
  auto set_trim(float start, float end) -> void;
  auto set_gain(float gain) -> void;
  auto set_pitch_controls(float pitch_bend, float vibrato_depth) -> void;
  auto trigger(float rate = 1.0F) -> void;

  [[nodiscard]] auto has_sample() const -> bool;
  [[nodiscard]] auto sample_frames() const -> std::size_t;
  [[nodiscard]] auto trim_start() const -> float;
  [[nodiscard]] auto trim_end() const -> float;
  [[nodiscard]] auto is_running() const -> bool;

private:
  struct Voice final {
    bool active{};
    float position{};
    float rate{1.0F};
  };

  static constexpr auto voice_count = std::size_t{8};
  static constexpr auto sample_rate = ma_uint32{48000};
  static constexpr auto channels = ma_uint32{2};

  ma_device device_{};
  std::vector<float> sample_{};
  std::array<Voice, voice_count> voices_{};
  mutable std::mutex mutex_{};
  float trim_start_{};
  float trim_end_{1.0F};
  float gain_{};
  float pitch_bend_{};
  float vibrato_depth_{};
  float vibrato_phase_{};
  bool device_ready_{};
  bool running_{};
  std::size_t next_voice_{};

  static auto playback_callback(ma_device *device, void *output,
                                const void *input, ma_uint32 frame_count)
      -> void;
};

} // namespace analogno
