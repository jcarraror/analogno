#include "command_handler.hpp"

#include "session.hpp"
#include "sf2_reader.hpp"
#include "transcribe_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace analogno {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

VoiceSequencerMode voice_mode_from_string(std::string_view mode) {
    if (mode == "harmonic") return VoiceSequencerMode::harmonic;
    if (mode == "hybrid")   return VoiceSequencerMode::hybrid;
    return VoiceSequencerMode::percussion;
}

TranscribeManager::SeqParams seq_params(const Sequencer& seq) {
    return {
        .bpm           = static_cast<float>(seq.bpm),
        .step_division = seq.step_division,
        .step_count    = seq.step_count,
    };
}

std::vector<float> resample_wavetable(const std::vector<float>& src,
                                       std::size_t target_n) {
    const auto src_n = src.size();
    std::vector<float> out(target_n);
    for (std::size_t i = 0; i < target_n; ++i) {
        const float t = static_cast<float>(i) *
                        static_cast<float>(src_n - 1) /
                        static_cast<float>(target_n - 1);
        const auto lo = static_cast<std::size_t>(t);
        const auto hi = std::min(lo + 1, src_n - 1);
        out[i] = src[lo] + (t - static_cast<float>(lo)) * (src[hi] - src[lo]);
    }
    return out;
}

} // namespace

void dispatch_web_commands(AppContext& ctx, const LoadStemsFn& load_stems_fn) {
    constexpr std::size_t wavetable_length = 367;

    for (auto& command : ctx.web.consume_commands()) {
        std::visit(Overloaded{

        // ── Panic ─────────────────────────────────────────────────────────
        [&](const WebSocketServer::Panic&) {
            MusicalIntent panic{};
            panic.note_off_all = true;
            ctx.midi.apply(panic);
            ctx.audio_sampler.stop_all();
            ctx.audio_sampler.stream_stop_all();
            ctx.active_notes.apply(panic);
            ctx.last_intent = panic;
            for (auto& rf : ctx.ribbon) {
                rf.active = false;
                rf.midi_note = -1;
                rf.active_channel = -1;
            }
            std::cout << "panic from web UI\n";
        },

        // ── Capture ───────────────────────────────────────────────────────
        [&](const WebSocketServer::SetCaptureDevice& cmd) {
            ctx.audio_capture.stop();
            if (cmd.device_index.has_value()) {
                ctx.audio_capture.start(
                    static_cast<std::uint32_t>(*cmd.device_index));
            } else {
                ctx.audio_capture.start();
            }
        },

        [&](const WebSocketServer::SetSampleTrim& cmd) {
            ctx.audio_sampler.set_trim(cmd.trim.start, cmd.trim.end);
        },

        // ── Bank / sampler ────────────────────────────────────────────────
        [&](const WebSocketServer::SetActiveBank& cmd) {
            ctx.audio_sampler.stop_all();
            ctx.audio_sampler.set_active_bank(cmd.bank);
            ctx.sampler_mode = ctx.audio_sampler.has_sample();
            std::cout << "active bank: " << cmd.bank
                      << " sampler_mode=" << (ctx.sampler_mode ? "on" : "off") << '\n';
        },

        [&](const WebSocketServer::SaveSample& cmd) {
            const char* home_env = std::getenv("HOME");
            const auto home = std::string{home_env != nullptr ? home_env : "."};
            const auto dir = home + "/analogno-samples";
            const auto epoch_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            const auto path = dir + "/bank-" + std::to_string(cmd.bank) + "-" +
                              std::to_string(epoch_ms) + ".wav";
            if (ctx.audio_sampler.save_bank(cmd.bank, path)) {
                std::cout << "saved bank " << cmd.bank << " to " << path << '\n';
            } else {
                std::cerr << "failed to save bank " << cmd.bank << '\n';
            }
        },

        [&](const WebSocketServer::SetBankRootNote& cmd) {
            if (cmd.bank >= 0 && cmd.bank < 8) {
                ctx.audio_sampler.set_bank_root_note(
                    static_cast<std::size_t>(cmd.bank), cmd.note);
                std::cout << "bank " << cmd.bank << " root: " << cmd.note << '\n';
            }
        },

        [&](const WebSocketServer::SetBankSliceCount& cmd) {
            if (cmd.bank >= 0 && cmd.bank < 8) {
                ctx.audio_sampler.set_bank_slice_count(
                    static_cast<std::size_t>(cmd.bank), cmd.count);
                std::cout << "bank " << cmd.bank << " slices: " << cmd.count << '\n';
            }
        },

        // ── Patch / MIDI program ──────────────────────────────────────────
        [&](const WebSocketServer::SetPatch& cmd) {
            ctx.sampler_mode = false;
            ctx.audio_sampler.stop_all();
            for (auto& rf : ctx.ribbon)
                release_ribbon_finger(rf, ctx.midi, ctx.active_notes);

            ctx.current_midi_bank    = cmd.patch.bank;
            ctx.current_midi_program = cmd.patch.program;

            const auto at = ctx.seq.active_track;
            if (at >= 0 && static_cast<std::size_t>(at) < ctx.seq.tracks.size()) {
                auto& active = ctx.seq.tracks[static_cast<std::size_t>(at)];
                active.midi_program = ctx.current_midi_program;
                active.midi_bank    = ctx.current_midi_bank;
                active.sample_bank  = -1;
                ctx.midi.program_change(ctx.current_midi_program,
                                        ctx.current_midi_bank,
                                        active.midi_channel);
                std::cout << "program: bank=" << ctx.current_midi_bank
                          << " prog=" << ctx.current_midi_program
                          << " ch=" << active.midi_channel << '\n';
            }
            for (auto& rf : ctx.ribbon) {
                ctx.midi.program_change(ctx.current_midi_program,
                                        ctx.current_midi_bank,
                                        ribbon_channel_for_bank(rf, ctx.current_midi_bank));
            }
        },

        // ── Wavetable ─────────────────────────────────────────────────────
        [&](WebSocketServer::SetWavetable& cmd) {
            ctx.sketch.committed = cmd.wavetable.samples;
            auto full = resample_wavetable(cmd.wavetable.samples, wavetable_length);
            std::vector<float> morph_full{};
            if (cmd.wavetable.morph_samples.size() >= 2)
                morph_full = resample_wavetable(cmd.wavetable.morph_samples, wavetable_length);
            ctx.audio_sampler.set_wavetable(std::move(full), std::move(morph_full));
            ctx.sampler_mode = true;
            std::cout << "wavetable set from UI\n";
        },

        [&](const WebSocketServer::SetWavetableControls& cmd) {
            ctx.wavetable_morph  = cmd.controls.morph;
            ctx.wavetable_noise  = cmd.controls.noise;
            ctx.wavetable_unison = cmd.controls.unison;
            ctx.audio_sampler.set_wavetable_controls(
                ctx.wavetable_morph, ctx.wavetable_noise, ctx.wavetable_unison);
        },

        // ── Sequencer ─────────────────────────────────────────────────────
        [&](const WebSocketServer::SeqPlay&) {
            ctx.seq.playing      = true;
            ctx.seq.current_step = -1;
            ctx.seq.playhead_step = -1;
            ctx.seq.step_start   = std::chrono::steady_clock::now();
            ctx.midi.send_start();
            std::cout << "seq: play\n";
        },

        [&](const WebSocketServer::SeqStop&) {
            ctx.seq.playing = false;
            for (auto& track : ctx.seq.tracks) {
                if (track.pending_note_off) {
                    ctx.midi.apply_notes_only({*track.pending_note_off}, {});
                    track.pending_note_off.reset();
                }
            }
            ctx.midi.send_stop();
            save_session(ctx.seq, ctx.stems_folder, ctx.session_path);
            std::cout << "seq: stop\n";
        },

        [&](const WebSocketServer::SeqAddTrack&) {
            if (static_cast<int>(ctx.seq.tracks.size()) < Sequencer::max_tracks) {
                std::vector<bool> used(16, false);
                for (const auto& t : ctx.seq.tracks) {
                    if (t.midi_channel >= 0 && t.midi_channel < 16)
                        used[static_cast<std::size_t>(t.midi_channel)] = true;
                }
                int new_ch = 0;
                while (new_ch < 16 && used[static_cast<std::size_t>(new_ch)])
                    ++new_ch;
                SeqTrack new_track{};
                new_track.midi_channel = new_ch < 16 ? new_ch : 0;
                new_track.loop_length  = ctx.seq.step_count;
                new_track.steps.resize(static_cast<std::size_t>(ctx.seq.step_count));
                ctx.seq.tracks.push_back(new_track);
                std::cout << "seq: added track ch=" << new_track.midi_channel << '\n';
            }
        },

        [&](const WebSocketServer::SeqRemoveTrack& cmd) {
            const auto idx = static_cast<std::size_t>(cmd.track);
            if (ctx.seq.tracks.size() > 1 && idx < ctx.seq.tracks.size()) {
                if (ctx.seq.tracks[idx].pending_note_off)
                    ctx.midi.apply_notes_only({*ctx.seq.tracks[idx].pending_note_off}, {});
                ctx.seq.tracks.erase(
                    ctx.seq.tracks.begin() + static_cast<std::ptrdiff_t>(idx));
                ctx.seq.active_track = std::clamp(ctx.seq.active_track, 0,
                    static_cast<int>(ctx.seq.tracks.size()) - 1);
                ctx.seq.selected_step = -1;
                for (const auto& t : ctx.seq.tracks)
                    ctx.midi.program_change(t.midi_program, t.midi_bank, t.midi_channel);
                std::cout << "seq: removed track " << idx << '\n';
            }
        },

        [&](const WebSocketServer::SeqSelectStep& cmd) {
            ctx.seq.selected_step =
                cmd.step < 0 ? -1
                             : std::clamp(cmd.step, 0,
                                          active_track_loop_length(ctx.seq) - 1);
            if (cmd.step >= 0)
                std::cout << "seq: arm step " << cmd.step << '\n';
            else
                std::cout << "seq: disarm\n";
        },

        [&](const WebSocketServer::SeqSelectTrack& cmd) {
            ctx.seq.active_track =
                std::clamp(cmd.track, 0,
                           static_cast<int>(ctx.seq.tracks.size()) - 1);
            ctx.seq.selected_step = -1;
            std::cout << "seq: track " << ctx.seq.active_track << '\n';
        },

        [&](const WebSocketServer::SetSeq& cmd) {
            const auto& cfg = cmd.config;
            ctx.seq.bpm            = cfg.bpm;
            ctx.seq.gate_pct       = cfg.gate_pct;
            ctx.seq.step_division  = sequencer_step_division(cfg.step_division);
            resize_sequencer_steps(ctx.seq, cfg.step_count);

            if (!cfg.tracks.empty()) {
                ctx.seq.tracks.resize(cfg.tracks.size());
                for (std::size_t t = 0; t < cfg.tracks.size(); ++t) {
                    auto& dst = ctx.seq.tracks[t];
                    const auto& src = cfg.tracks[t];
                    dst.steps.resize(static_cast<std::size_t>(ctx.seq.step_count));
                    if (src.midi_channel >= 0)
                        dst.midi_channel = src.midi_channel;
                    dst.sample_bank  = src.sample_bank;
                    dst.loop_length  = std::clamp(src.loop_length, 1, ctx.seq.step_count);
                    dst.muted        = src.muted;
                    const auto n = std::min(dst.steps.size(), src.steps.size());
                    for (std::size_t i = 0; i < n; ++i) {
                        const auto& sc = src.steps[i];
                        dst.steps[i] = {sc.active, sc.tie, sc.degree,
                                        sc.velocity, sc.midi_note};
                    }
                }
                ctx.seq.active_track = std::clamp(ctx.seq.active_track, 0,
                    static_cast<int>(ctx.seq.tracks.size()) - 1);
                if (ctx.seq.selected_step >= active_track_loop_length(ctx.seq))
                    ctx.seq.selected_step = -1;
                for (std::size_t t = 0; t < ctx.seq.tracks.size(); ++t) {
                    if (t < cfg.tracks.size()) {
                        const auto& src = cfg.tracks[t];
                        ctx.seq.tracks[t].volume         = std::clamp(src.volume, 0, 127);
                        ctx.seq.tracks[t].pan            = std::clamp(src.pan, 0, 127);
                        ctx.seq.tracks[t].velocity_scale = std::clamp(src.velocity_scale, 50, 200);
                        ctx.seq.tracks[t].solo           = src.solo;
                    }
                    const auto ch = effective_midi_channel(ctx.seq.tracks[t]);
                    ctx.midi.set_channel_volume(ch, ctx.seq.tracks[t].volume);
                    ctx.midi.set_channel_pan(ch, ctx.seq.tracks[t].pan);
                }
            }
        },

        // ── Track mixers ──────────────────────────────────────────────────
        [&](const WebSocketServer::SetTrackVolume& cmd) {
            const auto idx = static_cast<std::size_t>(cmd.track);
            if (idx < ctx.seq.tracks.size()) {
                ctx.seq.tracks[idx].volume = std::clamp(cmd.volume, 0, 127);
                ctx.midi.set_channel_volume(
                    effective_midi_channel(ctx.seq.tracks[idx]),
                    ctx.seq.tracks[idx].volume);
            }
        },

        [&](const WebSocketServer::SetTrackPan& cmd) {
            const auto idx = static_cast<std::size_t>(cmd.track);
            if (idx < ctx.seq.tracks.size()) {
                ctx.seq.tracks[idx].pan = std::clamp(cmd.pan, 0, 127);
                ctx.midi.set_channel_pan(
                    effective_midi_channel(ctx.seq.tracks[idx]),
                    ctx.seq.tracks[idx].pan);
            }
        },

        [&](const WebSocketServer::SetTrackVelocityScale& cmd) {
            const auto idx = static_cast<std::size_t>(cmd.track);
            if (idx < ctx.seq.tracks.size())
                ctx.seq.tracks[idx].velocity_scale = std::clamp(cmd.scale, 50, 200);
        },

        [&](const WebSocketServer::SetTrackSolo& cmd) {
            const auto idx = static_cast<std::size_t>(cmd.track);
            if (idx < ctx.seq.tracks.size())
                ctx.seq.tracks[idx].solo = cmd.solo;
        },

        [&](const WebSocketServer::SetSignalsVolume& cmd) {
            ctx.signals_volume = std::clamp(cmd.volume, 0.0F, 1.0F);
        },

        // ── Soundfont ─────────────────────────────────────────────────────
        [&](const WebSocketServer::SetSoundfont& cmd) {
            ctx.sampler_mode = false;
            ctx.audio_sampler.stop_all();
            for (auto& rf : ctx.ribbon)
                release_ribbon_finger(rf, ctx.midi, ctx.active_notes);

            ctx.active_soundfont = cmd.path;
            ctx.sf2_presets      = read_sf2_presets(ctx.active_soundfont);

            if (!ctx.sf2_presets.empty()) {
                ctx.current_midi_bank    = ctx.sf2_presets.front().bank;
                ctx.current_midi_program = ctx.sf2_presets.front().program;
                const auto at = ctx.seq.active_track;
                if (at >= 0 && static_cast<std::size_t>(at) < ctx.seq.tracks.size()) {
                    auto& active = ctx.seq.tracks[static_cast<std::size_t>(at)];
                    active.midi_bank    = ctx.current_midi_bank;
                    active.midi_program = ctx.current_midi_program;
                    active.sample_bank  = -1;
                    ctx.midi.program_change(ctx.current_midi_program,
                                            ctx.current_midi_bank,
                                            active.midi_channel);
                }
                for (auto& rf : ctx.ribbon) {
                    ctx.midi.program_change(ctx.current_midi_program,
                                            ctx.current_midi_bank,
                                            ribbon_channel_for_bank(rf, ctx.current_midi_bank));
                }
            }
            ctx.web.publish_library(make_web_library_state(
                ctx.sf2_presets, ctx.available_soundfonts, ctx.active_soundfont));
            std::cout << "soundfont: " << ctx.active_soundfont
                      << " (" << ctx.sf2_presets.size() << " presets)\n";
        },

        // ── Voice sequencer ───────────────────────────────────────────────
        [&](const WebSocketServer::SetVoiceSeq& cmd) {
            const auto& cfg = cmd.config;
            ctx.voice_seq.configure({
                .enabled           = cfg.enabled,
                .mode              = voice_mode_from_string(cfg.mode),
                .snap_to_scale     = cfg.snap_to_scale,
                .sensitivity       = cfg.sensitivity,
                .timing_offset_ms  = cfg.timing_offset_ms,
            });
            const auto was_recording = ctx.voice_seq.status().recording;
            if (cfg.recording && !was_recording) {
                ctx.voice_seq.start_recording(active_track_loop_length(ctx.seq));
            } else if (!cfg.recording && was_recording) {
                if (auto pattern = ctx.voice_seq.stop_recording(
                        ctx.last_intent.root_midi_note,
                        ctx.last_intent.octave_offset,
                        ctx.last_intent.scale)) {
                    apply_voice_pattern_to_seq(ctx.seq, *pattern);
                    std::cout << "voice seq: quantized " << pattern->segment_count
                              << " segments to track " << ctx.seq.active_track << '\n';
                }
            }
            std::cout << "voice seq: " << (cfg.enabled ? "on" : "off")
                      << " mode=" << cfg.mode
                      << " rec=" << (cfg.recording ? "on" : "off")
                      << " sens=" << cfg.sensitivity << '\n';
        },

        // ── Stems / streaming ─────────────────────────────────────────────
        [&](const WebSocketServer::StreamPlayBank& cmd) {
            ctx.audio_sampler.stream_play(cmd.bank);
        },

        [&](const WebSocketServer::StreamStopBank& cmd) {
            ctx.audio_sampler.stream_stop(cmd.bank);
        },

        [&](const WebSocketServer::StemPlay& cmd) {
            ctx.audio_sampler.stem_play(cmd.idx);
        },

        [&](const WebSocketServer::StemStop& cmd) {
            ctx.audio_sampler.stem_stop(cmd.idx);
        },

        [&](const WebSocketServer::SetActiveStem& cmd) {
            ctx.audio_sampler.set_active_stem(cmd.idx);
        },

        [&](const WebSocketServer::LoadStemToBank& cmd) {
            constexpr std::array<const char*, 6> labels{
                "drums","bass","vocals","guitar","piano","other"};
            if (static_cast<std::size_t>(cmd.stem_idx) >= labels.size() ||
                cmd.bank >= 8)
                return;
            const auto path = std::filesystem::path{ctx.stems_folder} /
                              (std::string{labels[static_cast<std::size_t>(cmd.stem_idx)]} + ".wav");
            if (!std::filesystem::exists(path)) return;
            ctx.audio_sampler.load_stem_to_bank(
                static_cast<std::size_t>(cmd.bank),
                path.string(), cmd.trim_start, cmd.trim_end);
            ctx.transcribe.invalidate_cache(static_cast<std::size_t>(cmd.bank));
            ctx.transcribe.invalidate_bank_seq(static_cast<std::size_t>(cmd.bank));
            ctx.audio_sampler.set_active_bank(static_cast<std::size_t>(cmd.bank));
            ctx.sampler_mode = true;
            if (ctx.seq.active_track >= 0 &&
                static_cast<std::size_t>(ctx.seq.active_track) < ctx.seq.tracks.size()) {
                ctx.seq.tracks[static_cast<std::size_t>(ctx.seq.active_track)].sample_bank =
                    static_cast<int>(cmd.bank);
            }
            std::cout << "[stems] " << labels[static_cast<std::size_t>(cmd.stem_idx)]
                      << " → bank " << cmd.bank << '\n';
        },

        [&](const WebSocketServer::SetStemFolder& cmd) {
            ctx.stems_folder = cmd.path;
            ctx.web.set_stems_folder(ctx.stems_folder);
            load_stems_fn(ctx.stems_folder);
            std::cout << "[stems] folder: " << ctx.stems_folder << '\n';
        },

        [&](const WebSocketServer::SplitAudioFile& cmd) {
            ctx.stem_pipeline.start(cmd.path);
        },

        [&](const WebSocketServer::DownloadAudio& cmd) {
            ctx.stem_pipeline.start(cmd.source);
        },

        // ── Transcription ─────────────────────────────────────────────────
        [&](const WebSocketServer::TranscribeBankToSeq& cmd) {
            if (cmd.bank < 0 || cmd.bank >= 8) return;
            if (ctx.transcribe.is_running()) return;
            if (ctx.seq.active_track < 0 ||
                static_cast<std::size_t>(ctx.seq.active_track) >= ctx.seq.tracks.size())
                return;
            const auto bidx       = static_cast<std::size_t>(cmd.bank);
            const auto first_track = static_cast<std::size_t>(ctx.seq.active_track);
            // Re-use the cached result when available — no need to re-analyse.
            if (const auto& cached = ctx.transcribe.cached(bidx)) {
                auto snap = apply_transcription(*cached, bidx, first_track,
                                                ctx.audio_sampler, ctx.seq, false);
                ctx.transcribe.store_bank_seq(bidx, std::move(snap));
                return;
            }
            ctx.transcribe.kick_bank(ctx.audio_sampler, bidx, first_track,
                                     seq_params(ctx.seq));
        },

        [&](const WebSocketServer::TranscribeMicToSeq&) {
            if (ctx.seq.active_track < 0 ||
                static_cast<std::size_t>(ctx.seq.active_track) >= ctx.seq.tracks.size())
                return;
            ctx.transcribe.kick_mic(
                ctx.audio_sampler.active_bank(),
                static_cast<std::size_t>(ctx.seq.active_track),
                seq_params(ctx.seq));
        },

        [&](const WebSocketServer::RevertToTranscribed& cmd) {
            if (cmd.bank < 0 || cmd.bank >= 8) return;
            if (ctx.seq.active_track < 0 ||
                static_cast<std::size_t>(ctx.seq.active_track) >= ctx.seq.tracks.size())
                return;
            if (ctx.transcribe.is_running()) return;
            const auto bidx = static_cast<std::size_t>(cmd.bank);
            if (const auto& cached = ctx.transcribe.cached(bidx)) {
                auto snap = apply_transcription(*cached, bidx,
                    static_cast<std::size_t>(ctx.seq.active_track),
                    ctx.audio_sampler, ctx.seq, false);
                ctx.transcribe.store_bank_seq(bidx, std::move(snap));
            } else {
                std::cout << "[transcribe] no cache for bank " << cmd.bank << '\n';
            }
        },

        [&](const WebSocketServer::LoadBankSeq& cmd) {
            if (cmd.bank < 0 || cmd.bank >= 8) return;
            const auto bidx = static_cast<std::size_t>(cmd.bank);
            if (!ctx.transcribe.has_bank_seq(bidx)) return;
            apply_bank_seq(ctx.transcribe.bank_seq(bidx), bidx, ctx.seq);
        },

        [&](const WebSocketServer::ArrangeBankToSeq& cmd) {
            if (cmd.bank < 0 || cmd.bank >= 8) return;
            if (ctx.transcribe.is_running()) return;
            const auto bidx        = static_cast<std::size_t>(cmd.bank);
            const auto first_track = ctx.transcribe.arrangement_first_track(bidx);
            ctx.transcribe.kick_bank(ctx.audio_sampler, bidx, first_track,
                                     seq_params(ctx.seq), true);
        },

        // ── File dialog ───────────────────────────────────────────────────
        [&](const WebSocketServer::OpenStemFolderDialog&) {
            const auto try_dialog = [](const char* cmd) -> std::string {
                FILE* pipe = popen(cmd, "r");
                if (!pipe) return {};
                char buf[4096]{};
                std::string result;
                if (fgets(buf, sizeof(buf), pipe)) result = buf;
                pclose(pipe);
                while (!result.empty() &&
                       (result.back() == '\n' || result.back() == '\r'))
                    result.pop_back();
                return result;
            };
            auto path = try_dialog(
                "zenity --file-selection --directory --title='Select stems folder' 2>/dev/null");
            if (path.empty())
                path = try_dialog("kdialog --getexistingdirectory / 2>/dev/null");
            if (!path.empty()) {
                ctx.stems_folder = path;
                ctx.web.set_stems_folder(ctx.stems_folder);
                load_stems_fn(ctx.stems_folder);
                std::cout << "[stems] folder: " << ctx.stems_folder << '\n';
            }
        },

        }, command); // end std::visit
    }
}

} // namespace analogno
