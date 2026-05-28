#pragma once

#include "audio_sampler.hpp"
#include "sequencer.hpp"
#include "stem_transcriber.hpp"

#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace analogno {

class TranscribeManager final {
public:
    struct SeqParams {
        float bpm{120.0F};
        int step_division{16};
        int step_count{32};
    };

    struct CompletedJob {
        TranscriptionResult result;
        std::size_t bank_idx{};
        std::size_t first_track{};
    };

    TranscribeManager() = default;
    ~TranscribeManager();

    TranscribeManager(const TranscribeManager&) = delete;
    TranscribeManager& operator=(const TranscribeManager&) = delete;
    TranscribeManager(TranscribeManager&&) = delete;
    TranscribeManager& operator=(TranscribeManager&&) = delete;

    void kick_bank(const AudioSampler& sampler,
                   std::size_t bank_idx,
                   std::size_t first_track,
                   const SeqParams& params);

    void kick_mic(std::size_t bank_idx,
                  std::size_t first_track,
                  const SeqParams& params);

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] std::optional<CompletedJob> poll();

    void set_mic_capture(std::vector<float> samples);
    [[nodiscard]] bool has_mic_capture() const noexcept;

    void invalidate_cache(std::size_t bank_idx) noexcept;
    void store_cache(std::size_t bank_idx, TranscriptionResult result);
    [[nodiscard]] const std::optional<TranscriptionResult>& cached(std::size_t bank_idx) const noexcept;
    [[nodiscard]] std::array<bool, AudioSampler::bank_count> cache_mask() const noexcept;

private:
    void launch(std::vector<float> samples,
                std::uint32_t channel_count,
                float trim_start,
                float trim_end,
                std::size_t bank_idx,
                std::size_t first_track,
                SeqParams params);

    std::thread thread_{};
    std::atomic<bool> running_{false};
    std::mutex result_mutex_{};
    std::optional<CompletedJob> pending_{};
    std::array<std::optional<TranscriptionResult>, AudioSampler::bank_count> cache_{};
    std::vector<float> mic_capture_{};
};

void apply_transcription(const TranscriptionResult& res,
                         std::size_t bank_idx,
                         std::size_t first_track,
                         AudioSampler& audio_sampler,
                         Sequencer& seq);

} // namespace analogno
