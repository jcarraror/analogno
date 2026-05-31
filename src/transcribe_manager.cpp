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
                                SeqParams params,
                                bool arrange) {
    if (thread_.joinable())
        thread_.join();
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread([this,
                           samples = std::move(samples),
                           channel_count, trim_start, trim_end,
                           bank_idx, first_track, params, arrange]() mutable {
        auto result = transcribe_to_seq(samples, channel_count,
                                        trim_start, trim_end,
                                        params.bpm, params.step_division,
                                        params.step_count);
        {
            const auto lock = std::scoped_lock{result_mutex_};
            pending_ = CompletedJob{std::move(result), bank_idx, first_track, arrange};
        }
        running_.store(false, std::memory_order_release);
    });
}

void TranscribeManager::kick_bank(const AudioSampler& sampler,
                                   std::size_t bank_idx,
                                   std::size_t first_track,
                                   const SeqParams& params,
                                   bool arrange) {
    if (running_.load(std::memory_order_acquire)) return;
    const auto snap = sampler.bank_snapshot(bank_idx);
    if (!snap.samples || snap.samples->empty()) {
        std::cout << "[transcribe] bank " << bank_idx << " has no audio\n";
        return;
    }
    std::cout << "[transcribe] analyzing bank " << bank_idx << " async...\n";
    launch(*snap.samples, snap.channel_count,
           snap.trim_start, snap.trim_end,
           bank_idx, first_track, params, arrange);
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
    launch(mic_capture_, 1, 0.0F, 1.0F, bank_idx, first_track, params, false);
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

BankSeqSnapshot apply_transcription(const TranscriptionResult& res,
                                     std::size_t bank_idx,
                                     std::size_t first_track,
                                     AudioSampler& audio_sampler,
                                     Sequencer& seq,
                                     bool expand) {
    if (res.tracks.empty() || res.onset_frames.empty()) return {};

    const auto audio_snap = audio_sampler.bank_snapshot(bank_idx);
    if (audio_snap.samples && !audio_snap.samples->empty()) {
        const auto bank_ch = std::max<std::size_t>(audio_snap.channel_count, 1U);
        const auto total_f = audio_snap.samples->size() / bank_ch;
        const auto trim_start_f =
            static_cast<double>(audio_snap.trim_start) * static_cast<double>(total_f);
        std::vector<double> onset_positions;
        onset_positions.reserve(res.onset_frames.size());
        for (const auto of : res.onset_frames)
            onset_positions.push_back(trim_start_f + of);
        audio_sampler.set_bank_onset_frames(bank_idx, std::move(onset_positions));
    }
    audio_sampler.set_bank_slice_count(bank_idx,
        static_cast<int>(res.onset_frames.size()));
    audio_sampler.set_bank_root_note(bank_idx, res.suggested_root_note);

    // Arrange mode: expand the sequencer so all generated tracks fit.
    // Transcribe mode: cap to however many tracks already exist from first_track onward.
    if (expand) {
        const auto needed = first_track + res.tracks.size();
        while (seq.tracks.size() < needed &&
               seq.tracks.size() < static_cast<std::size_t>(Sequencer::max_tracks)) {
            SeqTrack t{};
            t.midi_channel = static_cast<int>(seq.tracks.size() % 16);
            t.steps.assign(static_cast<std::size_t>(seq.step_count), {});
            seq.tracks.push_back(std::move(t));
        }
    }

    const auto n_tracks = std::min(res.tracks.size(), seq.tracks.size() - first_track);
    BankSeqSnapshot snap;
    snap.first_track = first_track;
    snap.loop_length  = res.suggested_loop_length;
    snap.track_steps.resize(n_tracks);
    snap.valid = true;

    for (std::size_t t = 0; t < n_tracks; ++t) {
        auto& track = seq.tracks[first_track + t];
        for (auto& step : track.steps) step = {};
        track.sample_bank  = static_cast<int>(bank_idx);
        track.loop_length  = res.suggested_loop_length;
        for (const auto& n : res.tracks[t]) {
            if (n.step < 0 || n.step >= seq.step_count) continue;
            const auto note_len =
                std::clamp(n.duration_steps, 1, std::max(1, seq.step_count - n.step));
            auto& step = track.steps[static_cast<std::size_t>(n.step)];
            step.active = true; step.tie = false;
            step.midi_note = n.midi_note; step.degree = n.onset_idx;
            step.velocity  = n.velocity;
            for (int hold = 1; hold < note_len; ++hold) {
                auto& tie = track.steps[static_cast<std::size_t>(n.step + hold)];
                tie.active = true; tie.tie = true;
                tie.midi_note = n.midi_note; tie.degree = n.onset_idx;
                tie.velocity  = n.velocity;
            }
        }
        snap.track_steps[t] = track.steps;
    }
    std::cout << "[transcribe] applied: " << res.onset_frames.size()
        << " onsets, " << n_tracks << " tracks, root=" << res.suggested_root_note << '\n';
    return snap;
}

void apply_bank_seq(const BankSeqSnapshot& snap, std::size_t bank_idx, Sequencer& seq) {
    if (!snap.valid || snap.track_steps.empty()) return;
    const auto n = std::min(snap.track_steps.size(), seq.tracks.size() - snap.first_track);
    for (std::size_t t = 0; t < n; ++t) {
        auto& track       = seq.tracks[snap.first_track + t];
        track.steps       = snap.track_steps[t];
        track.loop_length = snap.loop_length;
        track.sample_bank = static_cast<int>(bank_idx);
    }
    std::cout << "[transcribe] loaded bank " << bank_idx << " seq → tracks "
              << snap.first_track << "–" << (snap.first_track + n - 1) << '\n';
}

void TranscribeManager::store_bank_seq(std::size_t bank_idx, BankSeqSnapshot snap) {
    if (bank_idx < bank_seqs_.size())
        bank_seqs_[bank_idx] = std::move(snap);
}

void TranscribeManager::invalidate_bank_seq(std::size_t bank_idx) noexcept {
    if (bank_idx < bank_seqs_.size())
        bank_seqs_[bank_idx] = {};
}

bool TranscribeManager::has_bank_seq(std::size_t bank_idx) const noexcept {
    return bank_idx < bank_seqs_.size() && bank_seqs_[bank_idx].valid;
}

const BankSeqSnapshot& TranscribeManager::bank_seq(std::size_t bank_idx) const noexcept {
    static const BankSeqSnapshot empty{};
    return bank_idx < bank_seqs_.size() ? bank_seqs_[bank_idx] : empty;
}

std::array<bool, AudioSampler::bank_count>
TranscribeManager::bank_seq_mask() const noexcept {
    std::array<bool, AudioSampler::bank_count> mask{};
    for (std::size_t i = 0; i < AudioSampler::bank_count; ++i)
        mask[i] = bank_seqs_[i].valid;
    return mask;
}

std::array<int, AudioSampler::bank_count>
TranscribeManager::bank_seq_track_counts() const noexcept {
    std::array<int, AudioSampler::bank_count> counts{};
    for (std::size_t i = 0; i < AudioSampler::bank_count; ++i)
        counts[i] = bank_seqs_[i].valid
            ? static_cast<int>(bank_seqs_[i].track_steps.size())
            : 0;
    return counts;
}

std::array<int, AudioSampler::bank_count>
TranscribeManager::cached_track_counts() const noexcept {
    std::array<int, AudioSampler::bank_count> counts{};
    for (std::size_t i = 0; i < AudioSampler::bank_count; ++i)
        counts[i] = cache_[i].has_value()
            ? static_cast<int>(cache_[i]->tracks.size())
            : 0;
    return counts;
}

std::size_t TranscribeManager::arrangement_first_track(std::size_t bank_idx) const noexcept {
    if (has_bank_seq(bank_idx))
        return bank_seqs_[bank_idx].first_track;
    std::size_t end = 0;
    for (std::size_t i = 0; i < AudioSampler::bank_count; ++i) {
        if (i == bank_idx || !bank_seqs_[i].valid) continue;
        end = std::max(end, bank_seqs_[i].first_track + bank_seqs_[i].track_steps.size());
    }
    return end;
}

} // namespace analogno
