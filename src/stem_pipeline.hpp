#pragma once

#include "audio_downloader.hpp"
#include "audio_sampler.hpp"
#include "stem_splitter.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace analogno {

class StemPipeline final {
public:
    static constexpr std::array<const char*, 6> stem_labels{
        "drums", "bass", "vocals", "guitar", "piano", "other"};
    static constexpr std::size_t max_stem_folders = 5;

    struct Status {
        std::string state{"idle"};
        float progress{};
        std::string detail{};
        std::vector<std::string> log{};
        std::string error{};
    };

    StemPipeline() = default;

    StemPipeline(const StemPipeline&) = delete;
    StemPipeline& operator=(const StemPipeline&) = delete;
    StemPipeline(StemPipeline&&) = delete;
    StemPipeline& operator=(StemPipeline&&) = delete;

    void start(const std::string& source);
    void mark_stems_ready();

    // Poll per-frame. Returns the new stems folder path when stems are freshly ready.
    [[nodiscard]] std::optional<std::string> tick(const std::string& base_dir);

    [[nodiscard]] const Status& status() const noexcept { return status_; }

private:
    [[nodiscard]] std::optional<std::string> handle_split_done(const std::string& base_dir);
    void prune_old_folders(const std::filesystem::path& base);

    StemSplitter splitter_{};
    AudioDownloader downloader_{};
    Status status_{};
};

[[nodiscard]] std::size_t load_stems(const std::string& folder, AudioSampler& sampler);

} // namespace analogno
