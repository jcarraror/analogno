#include "wav_stream.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <limits>
#include <thread>

namespace analogno {

WavStream::WavStream() : ring_(ring_frames * 2, 0.0F) {}

WavStream::~WavStream() { stop(); }

void WavStream::start(std::string path, float trim_start, float trim_end) {
  stop();

  write_pos_.store(0, std::memory_order_relaxed);
  read_pos_.store(0, std::memory_order_relaxed);
  eof_.store(false, std::memory_order_relaxed);
  stop_flag_.store(false, std::memory_order_relaxed);
  std::fill(ring_.begin(), ring_.end(), 0.0F);

  active_.store(true, std::memory_order_release);
  reader_ = std::thread(&WavStream::reader_func, this,
                        std::move(path), trim_start, trim_end);
}

void WavStream::stop() {
  stop_flag_.store(true, std::memory_order_release);
  if (reader_.joinable()) {
    reader_.join();
  }
  active_.store(false, std::memory_order_release);
}

bool WavStream::read_frame(float &l, float &r) {
  const auto w = write_pos_.load(std::memory_order_acquire);
  const auto rd = read_pos_.load(std::memory_order_relaxed);

  if (rd >= w) {
    if (eof_.load(std::memory_order_acquire)) {
      active_.store(false, std::memory_order_release);
      return false;
    }
    l = r = 0.0F;
    return true;
  }

  const auto pos = rd % ring_frames;
  l = ring_[pos * 2];
  r = ring_[pos * 2 + 1];
  read_pos_.store(rd + 1, std::memory_order_release);
  return true;
}

void WavStream::reader_func(std::string path, float trim_start, float trim_end) {
  auto cfg = ma_decoder_config_init(ma_format_f32, 2, device_rate);
  ma_decoder dec{};
  if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) {
    std::cerr << "[wav_stream] failed to open: " << path << '\n';
    eof_.store(true, std::memory_order_release);
    return;
  }

  ma_uint64 total_frames = 0;
  ma_decoder_get_length_in_pcm_frames(&dec, &total_frames);

  const ma_uint64 start_frame = total_frames > 0
      ? static_cast<ma_uint64>(trim_start * static_cast<float>(total_frames))
      : 0;
  const ma_uint64 end_frame = total_frames > 0
      ? static_cast<ma_uint64>(trim_end * static_cast<float>(total_frames))
      : std::numeric_limits<ma_uint64>::max();

  if (start_frame > 0) {
    ma_decoder_seek_to_pcm_frame(&dec, start_frame);
  }

  constexpr std::size_t chunk = 2048;
  std::array<float, chunk * 2> temp{};
  ma_uint64 frames_decoded = 0;
  const ma_uint64 frames_to_read = end_frame > start_frame
      ? end_frame - start_frame
      : std::numeric_limits<ma_uint64>::max();

  while (!stop_flag_.load(std::memory_order_relaxed) &&
         frames_decoded < frames_to_read) {
    // Wait for space in the ring buffer
    for (;;) {
      if (stop_flag_.load(std::memory_order_relaxed)) goto done;
      const auto w = write_pos_.load(std::memory_order_acquire);
      const auto rd = read_pos_.load(std::memory_order_acquire);
      if (w - rd < ring_frames - chunk) break;
      std::this_thread::sleep_for(std::chrono::microseconds{500});
    }

    const auto to_read = static_cast<ma_uint64>(
        std::min(static_cast<ma_uint64>(chunk), frames_to_read - frames_decoded));

    ma_uint64 frames_read = 0;
    ma_decoder_read_pcm_frames(&dec, temp.data(), to_read, &frames_read);
    if (frames_read == 0) break;

    const auto w = write_pos_.load(std::memory_order_relaxed);
    for (ma_uint64 i = 0; i < frames_read; ++i) {
      const auto pos = (w + i) % ring_frames;
      ring_[pos * 2] = temp[i * 2];
      ring_[pos * 2 + 1] = temp[i * 2 + 1];
    }
    write_pos_.fetch_add(frames_read, std::memory_order_release);
    frames_decoded += frames_read;
  }

done:
  ma_decoder_uninit(&dec);
  eof_.store(true, std::memory_order_release);
}

} // namespace analogno
