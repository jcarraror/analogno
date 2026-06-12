#include "live_router.hpp"

#include "app_context.hpp"
#include "sequencer.hpp"

#include <SDL3/SDL_gamepad.h>
#include <algorithm>
#include <cmath>

namespace analogno {

void route_live_notes(AppContext& ctx, const AudioFeatures& audio_features) {
    if (!ctx.controller.changed_this_frame()) return;

    auto intent = ctx.mapper.map(ctx.controller);

    const auto at = ctx.seq.active_track;
    const int live_ch =
        (at >= 0 && static_cast<std::size_t>(at) < ctx.seq.tracks.size())
        ? effective_midi_channel(ctx.seq.tracks[static_cast<std::size_t>(at)])
        : 0;

    for (auto& n : intent.note_ons)  n.channel = live_ch;
    for (auto& n : intent.note_offs) n.channel = live_ch;

    const bool l2_gate    = ctx.controller.left_trigger() > 0.05F;
    const auto raw_note_ons = intent.note_ons;
    const auto active_sb  = ctx.audio_sampler.active_bank();
    const int active_track_sbank =
        (at >= 0 && static_cast<std::size_t>(at) < ctx.seq.tracks.size())
        ? ctx.seq.tracks[static_cast<std::size_t>(at)].sample_bank
        : -1;
    const int live_asb =
        (ctx.sampler_mode && ctx.audio_sampler.bank_has_sample(active_sb))
        ? static_cast<int>(active_sb)
        : active_track_sbank;

    if (live_asb >= 0 &&
        ctx.audio_sampler.bank_has_sample(static_cast<std::size_t>(live_asb))) {
        const auto asb = static_cast<std::size_t>(live_asb);
        if (intent.note_off_all) {
            ctx.audio_sampler.stop_all();
            ctx.audio_sampler.stream_stop_all();
        }
        if (ctx.audio_sampler.bank_is_stream(asb)) {
            const auto slice_count = ctx.audio_sampler.bank_slice_count(asb);
            if (l2_gate && !intent.note_ons.empty()) {
                if (slice_count > 0) {
                    const auto degree = intent.note_ons[0].degree;
                    const auto page   = ctx.audio_sampler.get_slice_page(asb);
                    ctx.audio_sampler.stream_play_slice(asb, page + degree);
                } else {
                    const auto root      = ctx.audio_sampler.bank_root_note(asb);
                    const auto semitones = intent.note_ons[0].midi_note - root;
                    ctx.audio_sampler.stream_play(
                        asb, std::pow(2.0F, static_cast<float>(semitones) / 12.0F));
                }
            }
            if (slice_count > 0 && ctx.controller.button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) {
                const auto page = ctx.audio_sampler.get_slice_page(asb);
                if (ctx.controller.button_pressed(SDL_GAMEPAD_BUTTON_DPAD_UP))
                    ctx.audio_sampler.set_slice_page(asb, page - 4);
                if (ctx.controller.button_pressed(SDL_GAMEPAD_BUTTON_DPAD_DOWN))
                    ctx.audio_sampler.set_slice_page(asb, page + 4);
                if (ctx.controller.button_pressed(SDL_GAMEPAD_BUTTON_DPAD_LEFT))
                    ctx.audio_sampler.set_slice_page(asb, 0);
                if (ctx.controller.button_pressed(SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
                    ctx.audio_sampler.set_slice_page(asb,
                        std::max(0, slice_count - 4));
            }
            if (!intent.note_offs.empty())
                ctx.audio_sampler.stream_stop(asb);
        } else {
            for (const auto& note : intent.note_offs) {
                const auto root      = ctx.audio_sampler.bank_root_note(asb);
                const auto semitones = note.midi_note - root;
                ctx.audio_sampler.release_bank(
                    asb, std::pow(2.0F, static_cast<float>(semitones) / 12.0F),
                    note.midi_note);
            }
            if (l2_gate) {
                for (const auto& note : intent.note_ons) {
                    const auto root      = ctx.audio_sampler.bank_root_note(asb);
                    const auto semitones = note.midi_note - root;
                    ctx.audio_sampler.trigger_bank(
                        asb, std::pow(2.0F, static_cast<float>(semitones) / 12.0F),
                        note.midi_note);
                }
            }
        }
        ctx.active_notes.apply(intent);
    } else {
        if (!l2_gate) intent.note_ons.clear();
        ctx.midi.set_live_channel(live_ch);
        ctx.midi.apply(intent);
        ctx.active_notes.apply(intent);
    }

    ctx.last_intent         = intent;
    ctx.last_audio_features = audio_features;

    if (ctx.seq.selected_step >= 0 && !raw_note_ons.empty()) {
        const auto& n   = raw_note_ons[0];
        const auto tidx = static_cast<std::size_t>(ctx.seq.active_track);
        const auto sidx = static_cast<std::size_t>(ctx.seq.selected_step);
        ctx.seq.tracks[tidx].steps[sidx] = {true, false, n.degree, n.velocity, n.midi_note};
        ctx.seq.selected_step =
            (ctx.seq.selected_step + 1) % active_track_loop_length(ctx.seq);
    }
    ctx.controller.clear_frame_edges();
}

} // namespace analogno
