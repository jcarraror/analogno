#include "transcribe_manager.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <utility>

namespace analogno {

TranscribeManager::~TranscribeManager() {
    if (thread_.joinable())
        thread_.join();
}

void TranscribeManager::launch(std::vector<float> samples,
                                std::uint32_t channel_count,
                                float trim_start,
                                float trim_end,
                                std::size_t bank_idx,
                                std::size_t first_track,
                                SeqParams params) {
    if (thread_.joinable())
        thread_.join();
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread([this,
                           samples = std::move(samples),
                           channel_count, trim_start, trim_end,
                           bank_idx, first_track, params]() mutable {
        auto result = transcribe_to_seq(samples, channel_count,
                                        trim_start, trim_end,
                                        params.bpm, params.step_division,
                                        params.step_count);
        {
            const auto lock = std::scoped_lock{result_mutex_};
            pending_ = CompletedJob{std::move(result), bank_idx, first_track};
        }
        running_.store(false, std::memory_order_release);
    });
}

void TranscribeManager::kick_bank(const AudioSampler& sampler,
                                   std::size_t bank_idx,
                                   std::size_t first_track,
                                   const SeqParams& params) {
    if (running_.load(std::memory_order_acquire)) return;
    const auto snap = sampler.bank_snapshot(bank_idx);
    if (!snap.samples || snap.samples->empty()) {
        std::cout << "[transcribe] bank " << bank_idx << " has no audio\n";
        return;
    }
    std::cout << "[transcribe] analyzing bank " << bank_idx << " async...\n";
    launch(*snap.samples, snap.channel_count,
           snap.trim_start, snap.trim_end,
           bank_idx, first_track, params);
}

void TranscribeManager::kick_mic(std::size_t bank_idx,
                                  std::size_t first_track,
                                  const SeqParams& params) {
    if (running_.load(std::memory_order_acquire)) return;
    if (mic_capture_.empty()) {
        std::cout << "[transcribe] no mic capture\n";
        return;
    }
    std::cout << "[transcribe] analyzing mic capture async...\n";
    launch(mic_capture_, 1, 0.0F, 1.0F, bank_idx, first_track, params);
}

bool TranscribeManager::is_running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

std::optional<TranscribeManager::CompletedJob> TranscribeManager::poll() {
    if (running_.load(std::memory_order_acquire)) return std::nullopt;
    const auto lock = std::scoped_lock{result_mutex_};
    return std::exchange(pending_, std::nullopt);
}

void TranscribeManager::set_mic_capture(std::vector<float> samples) {
    mic_capture_ = std::move(samples);
}

bool TranscribeManager::has_mic_capture() const noexcept {
    return !mic_capture_.empty();
}

void TranscribeManager::invalidate_cache(std::size_t bank_idx) noexcept {
    if (bank_idx < cache_.size())
        cache_[bank_idx].reset();
}

void TranscribeManager::store_cache(std::size_t bank_idx, TranscriptionResult result) {
    if (bank_idx < cache_.size())
        cache_[bank_idx] = std::move(result);
}

const std::optional<TranscriptionResult>&
TranscribeManager::cached(std::size_t bank_idx) const noexcept {
    static const std::optional<TranscriptionResult> empty{};
    return bank_idx < cache_.size() ? cache_[bank_idx] : empty;
}

std::array<bool, AudioSampler::bank_count>
TranscribeManager::cache_mask() const noexcept {
    std::array<bool, AudioSampler::bank_count> mask{};
    for (std::size_t i = 0; i < AudioSampler::bank_count; ++i)
        mask[i] = cache_[i].has_value();
    return mask;
}

void apply_transcription(const TranscriptionResult& res,
                          std::size_t bank_idx,
                          std::size_t first_track,
                          AudioSampler& audio_sampler,
                          Sequencer& seq) {
    if (res.tracks.empty() || res.onset_frames.empty()) return;

    const auto snap = audio_sampler.bank_snapshot(bank_idx);
    if (snap.samples && !snap.samples->empty()) {
        const auto bank_ch = std::max<std::size_t>(snap.channel_count, 1U);
        const auto total_f = snap.samples->size() / bank_ch;
        const auto trim_start_f =
            static_cast<double>(snap.trim_start) * static_cast<double>(total_f);
        std::vector<double> onset_positions;
        onset_positions.reserve(res.onset_frames.size());
        for (const auto of : res.onset_frames)
            onset_positions.push_back(trim_start_f + of);
        audio_sampler.set_bank_onset_frames(bank_idx, std::move(onset_positions));
    }
    audio_sampler.set_bank_slice_count(bank_idx,
        static_cast<int>(res.onset_frames.size()));
    audio_sampler.set_bank_root_note(bank_idx, res.suggested_root_note);

    const auto n_tracks = std::min(res.tracks.size(), seq.tracks.size() - first_track);
    for (std::size_t t = 0; t < n_tracks; ++t) {
        auto& track = seq.tracks[first_track + t];
        for (auto& step : track.steps) step = {};
        track.sample_bank = static_cast<int>(bank_idx);
        track.loop_length = res.suggested_loop_length;
        for (const auto& n : res.tracks[t]) {
            if (n.step < 0 || n.step >= seq.step_count) continue;
            const auto note_len =
                std::clamp(n.duration_steps, 1, std::max(1, seq.step_count - n.step));
            auto& step = track.steps[static_cast<std::size_t>(n.step)];
            step.active = true; step.tie = false;
            step.midi_note = n.midi_note; step.degree = n.onset_idx;
            step.velocity = n.velocity;
            for (int hold = 1; hold < note_len; ++hold) {
                auto& tie = track.steps[static_cast<std::size_t>(n.step + hold)];
                tie.active = true; tie.tie = true;
                tie.midi_note = n.midi_note; tie.degree = n.onset_idx;
                tie.velocity = n.velocity;
            }
        }
    }
    std::cout << "[transcribe] applied: " << res.onset_frames.size()
        << " onsets, " << n_tracks << " tracks, root=" << res.suggested_root_note << '\n';
}

} // namespace analogno
