#include "audio_downloader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace analogno {

namespace fs = std::filesystem;

static constexpr const char *k_out_dir = "/tmp/analogno-dl";

static std::string shell_quote(const std::string &value) {
  std::string q{"'"};
  for (const char c : value) {
    if (c == '\'') {
      q += "'\\''";
    } else {
      q += c;
    }
  }
  q += "'";
  return q;
}

static std::string find_ytdlp() {
  if (const char *e = std::getenv("ANALOGNO_YTDLP"); e && *e != '\0') {
    return shell_quote(std::string{e});
  }
  for (const auto &p :
       {fs::path{".venv/bin/yt-dlp"}, fs::path{"../.venv/bin/yt-dlp"}}) {
    if (fs::exists(p)) {
      return shell_quote(p.string());
    }
  }
  return "yt-dlp";
}

static std::optional<float> parse_percent(std::string_view line) {
  const auto pct = line.find('%');
  if (pct == std::string_view::npos) {
    return std::nullopt;
  }
  auto i = pct;
  while (i > 0 && (line[i - 1] == '.' ||
                   std::isdigit(static_cast<unsigned char>(line[i - 1])))) {
    --i;
  }
  if (i == pct) {
    return std::nullopt;
  }
  float val = 0.0F;
  float frac_mul = 0.1F;
  bool in_frac = false;
  bool found = false;
  for (auto j = i; j < pct; ++j) {
    const char c = line[j];
    if (c == '.') {
      in_frac = true;
      continue;
    }
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      continue;
    }
    found = true;
    if (in_frac) {
      val += static_cast<float>(c - '0') * frac_mul;
      frac_mul *= 0.1F;
    } else {
      val = val * 10.0F + static_cast<float>(c - '0');
    }
  }
  return found ? std::optional{val} : std::nullopt;
}

bool AudioDownloader::is_url(const std::string &source) {
  return source.starts_with("http://") || source.starts_with("https://") ||
         source.starts_with("www.");
}

AudioDownloader::~AudioDownloader() {
  if (thread_.joinable()) {
    thread_.join();
  }
}

void AudioDownloader::download(std::string source) {
  if (state_.load() == DownloadState::running) {
    return;
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  {
    const auto lock = std::scoped_lock{result_mutex_};
    result_.reset();
    error_.reset();
  }

  if (!is_url(source)) {
    const auto path = fs::path{source};
    if (fs::exists(path)) {
      {
        const auto lock = std::scoped_lock{result_mutex_};
        result_ = path;
      }
      set_progress(1.0F, "File ready");
      state_.store(DownloadState::done);
    } else {
      {
        const auto lock = std::scoped_lock{result_mutex_};
        error_ = "file not found: " + source;
      }
      set_progress(0.0F, "File not found");
      state_.store(DownloadState::error);
    }
    return;
  }

  set_progress(0.02F, "Starting yt-dlp");
  state_.store(DownloadState::running);

  thread_ = std::thread([this, source = std::move(source)] {
    std::error_code ec;
    fs::create_directories(k_out_dir, ec);

    // Remove any previous download so we can reliably find the new one
    for (const auto *ext :
         {".mp3", ".wav", ".flac", ".ogg", ".m4a", ".webm", ".opus"}) {
      const auto p = fs::path{k_out_dir} / (std::string{"download"} + ext);
      fs::remove(p, ec);
    }

    const auto out_template =
        std::string{k_out_dir} + "/download.%(ext)s";

    // --newline forces one progress line per stdout write (required for popen)
    const std::string cmd =
        find_ytdlp() +
        " -x"
        " --audio-format mp3"
        " --audio-quality 0"
        " --no-playlist"
        " --newline"
        " -o " + shell_quote(out_template) +
        " " + shell_quote(source) +
        " 2>&1";

    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
      const auto lock = std::scoped_lock{result_mutex_};
      error_ = "failed to spawn yt-dlp — is it installed?";
      set_progress(0.0F, "yt-dlp not found");
      state_.store(DownloadState::error);
      return;
    }

    std::array<char, 1024> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
      std::cout << "[ytdlp] " << buf.data();
      std::string_view line{buf.data()};
      while (!line.empty() &&
             (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
      }
      if (line.empty()) {
        continue;
      }

      append_log(std::string{line});

      if (line.starts_with("[download]")) {
        if (const auto pct = parse_percent(line)) {
          set_progress(*pct / 100.0F * 0.92F, "Downloading…");
        }
      } else if (line.find("ffmpeg") != std::string_view::npos ||
                 line.find("Destination") != std::string_view::npos) {
        set_progress(0.93F, "Converting to mp3…");
      }
    }
    std::cout << std::flush;

    if (pclose(pipe) != 0) {
      const auto lock = std::scoped_lock{result_mutex_};
      error_ = "yt-dlp exited with an error — check the [ytdlp] logs";
      set_progress(progress_.load(), "Download failed");
      state_.store(DownloadState::error);
      return;
    }

    // Find the output file (format may vary even with --audio-format mp3)
    std::optional<fs::path> out_path;
    for (const auto *ext :
         {".mp3", ".wav", ".flac", ".ogg", ".m4a", ".webm", ".opus"}) {
      const auto p = fs::path{k_out_dir} / (std::string{"download"} + ext);
      if (fs::exists(p)) {
        out_path = p;
        break;
      }
    }

    if (!out_path) {
      const auto lock = std::scoped_lock{result_mutex_};
      error_ = "yt-dlp finished but output file not found in " +
               std::string{k_out_dir};
      set_progress(progress_.load(), "Output file missing");
      state_.store(DownloadState::error);
      return;
    }

    {
      const auto lock = std::scoped_lock{result_mutex_};
      result_ = *out_path;
    }
    set_progress(1.0F, "Download complete");
    state_.store(DownloadState::done);
  });
}

void AudioDownloader::reset() {
  if (thread_.joinable()) {
    thread_.join();
  }
  {
    const auto lock = std::scoped_lock{result_mutex_};
    result_.reset();
    error_.reset();
  }
  {
    const auto lock = std::scoped_lock{progress_mutex_};
    log_lines_.clear();
    detail_.clear();
  }
  progress_.store(0.0F, std::memory_order_relaxed);
  state_.store(DownloadState::idle);
}

DownloadState AudioDownloader::state() const { return state_.load(); }

float AudioDownloader::progress() const { return progress_.load(); }

std::string AudioDownloader::detail() const {
  const auto lock = std::scoped_lock{progress_mutex_};
  return detail_;
}

void AudioDownloader::set_progress(float p, std::string detail) {
  const auto clamped = std::clamp(p, 0.0F, 1.0F);
  float current = progress_.load(std::memory_order_relaxed);
  while (clamped > current &&
         !progress_.compare_exchange_weak(current, clamped,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
  }
  const auto lock = std::scoped_lock{progress_mutex_};
  detail_ = std::move(detail);
}

void AudioDownloader::append_log(std::string line) {
  const auto lock = std::scoped_lock{progress_mutex_};
  log_lines_.push_back(std::move(line));
  while (log_lines_.size() > k_max_log_lines) {
    log_lines_.pop_front();
  }
}

std::vector<std::string> AudioDownloader::log() const {
  const auto lock = std::scoped_lock{progress_mutex_};
  return {log_lines_.begin(), log_lines_.end()};
}

std::optional<fs::path> AudioDownloader::take_result() {
  const auto lock = std::scoped_lock{result_mutex_};
  return std::exchange(result_, std::nullopt);
}

std::optional<std::string> AudioDownloader::take_error() {
  const auto lock = std::scoped_lock{result_mutex_};
  return std::exchange(error_, std::nullopt);
}

} // namespace analogno
