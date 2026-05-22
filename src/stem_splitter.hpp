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

enum class StemSplitState { idle, running, done, error };

struct StemPaths {
    std::filesystem::path drums;
    std::filesystem::path bass;
    std::filesystem::path vocals;
    std::filesystem::path guitar;
    std::filesystem::path piano;
    std::filesystem::path other;
};

class StemSplitter final {
public:
    StemSplitter() = default;
    ~StemSplitter();

    StemSplitter(const StemSplitter &) = delete;
    StemSplitter &operator=(const StemSplitter &) = delete;
    StemSplitter(StemSplitter &&) = delete;
    StemSplitter &operator=(StemSplitter &&) = delete;

    void split(std::filesystem::path input);
    void reset();

    [[nodiscard]] StemSplitState state() const;
    [[nodiscard]] float progress() const;
    [[nodiscard]] std::string detail() const;
    [[nodiscard]] std::vector<std::string> log() const;
    [[nodiscard]] std::optional<StemPaths> take_result();
    [[nodiscard]] std::optional<std::string> take_error();

private:
    void set_progress(float progress, std::string detail);
    void append_log(std::string line);

    static constexpr std::size_t k_max_log_lines = 30;

    std::atomic<StemSplitState> state_{StemSplitState::idle};
    std::atomic<float> progress_{0.0F};
    std::thread thread_{};
    std::mutex result_mutex_{};
    mutable std::mutex progress_mutex_{};
    std::string detail_{};
    std::deque<std::string> log_lines_{};
    std::optional<StemPaths> result_{};
    std::optional<std::string> error_{};
};

} // namespace analogno
