#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace analogno {

class WavStream {
public:
  static constexpr std::size_t ring_frames = 96000; // 2-second ring at 48 kHz

  WavStream();
  ~WavStream();

  WavStream(const WavStream &) = delete;
  WavStream &operator=(const WavStream &) = delete;

  void start(std::string path, float trim_start, float trim_end);
  void stop();

  [[nodiscard]] bool is_active() const {
    return active_.load(std::memory_order_relaxed);
  }

  // Real-time safe — called from audio callback only.
  // Returns false when the stream ends; sets is_active() to false.
  bool read_frame(float &left, float &right);

private:
  static constexpr std::uint32_t device_rate = 48000;

  std::vector<float> ring_;
  std::atomic<std::uint64_t> write_pos_{0};
  std::atomic<std::uint64_t> read_pos_{0};
  std::atomic<bool> eof_{false};
  std::atomic<bool> stop_flag_{false};
  std::atomic<bool> active_{false};
  std::thread reader_;

  void reader_func(std::string path, float trim_start, float trim_end);
};

} // namespace analogno
