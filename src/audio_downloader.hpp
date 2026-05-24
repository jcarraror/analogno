#pragma once

#include <atomic>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace analogno {

enum class DownloadState { idle, running, done, error };

class AudioDownloader final {
public:
  AudioDownloader() = default;
  ~AudioDownloader();

  AudioDownloader(const AudioDownloader &) = delete;
  AudioDownloader &operator=(const AudioDownloader &) = delete;
  AudioDownloader(AudioDownloader &&) = delete;
  AudioDownloader &operator=(AudioDownloader &&) = delete;

  void download(std::string source);
  void reset();

  [[nodiscard]] DownloadState state() const;
  [[nodiscard]] float progress() const;
  [[nodiscard]] std::string detail() const;
  [[nodiscard]] std::vector<std::string> log() const;
  [[nodiscard]] std::optional<std::filesystem::path> take_result();
  [[nodiscard]] std::optional<std::string> take_error();

  [[nodiscard]] static bool is_url(const std::string &source);

private:
  void set_progress(float p, std::string detail);
  void append_log(std::string line);

  static constexpr std::size_t k_max_log_lines = 30;

  std::atomic<DownloadState> state_{DownloadState::idle};
  std::atomic<float> progress_{0.0F};
  std::thread thread_{};
  mutable std::mutex progress_mutex_{};
  std::mutex result_mutex_{};
  std::string detail_{};
  std::deque<std::string> log_lines_{};
  std::optional<std::filesystem::path> result_{};
  std::optional<std::string> error_{};
};

} // namespace analogno
