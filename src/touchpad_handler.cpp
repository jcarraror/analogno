#include "touchpad_handler.hpp"

#include "app_context.hpp"
#include "runtime_state_builder.hpp"
#include "sequencer.hpp"

#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace analogno {

namespace {

constexpr std::size_t wavetable_length = 367;

[[nodiscard]] int ribbon_note_for_x(float x, int root, int oct_off, ScaleKind sk) {
    const auto sc   = scale_for(sk);
    const int total = sc.size * 2;
    const int idx   = std::clamp(static_cast<int>(x * static_cast<float>(total)), 0, total - 1);
    const int oct   = idx / sc.size;
    const int step  = idx % sc.size;
    return std::clamp(root + oct_off * 12 + oct * 12 +
                      sc.semitones[static_cast<std::size_t>(step)], 0, 127);
}

} // namespace

void handle_touchpad_event(const SDL_Event& event, AppContext& ctx) {
    if (event.gtouchpad.touchpad != 0) return;

    const auto fi    = static_cast<std::size_t>(event.gtouchpad.finger);
    const bool l1    = ctx.controller.button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    const bool r1    = ctx.controller.button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    const bool click = ctx.controller.button(SDL_GAMEPAD_BUTTON_TOUCHPAD);
    constexpr std::size_t ribbon_max = 2;

    if (event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN) {
        if (fi == 0 && ctx.seq.selected_step >= 0 && !l1 && !click && !r1) {
            ctx.tp_swipe = {event.gtouchpad.x, event.gtouchpad.y, true};

        } else if (fi == 0 && l1 && !click) {
            if (!ctx.sketch.active) { ctx.sketch.active = true; ctx.sketch.points.clear(); }
            ctx.sketch.points.emplace_back(event.gtouchpad.x, event.gtouchpad.y);

        } else if (fi < ribbon_max && r1 && !l1 && ctx.current_midi_bank == 128) {
            if (ctx.drum_pad.in_nav_strip(event.gtouchpad.y)) {
                ctx.drum_pad.navigate(event.gtouchpad.x >= 0.5F);
                std::cout << "drum page: " << ctx.drum_pad.page << '\n';
            } else {
                const int note = ctx.drum_pad.note_for_xy(event.gtouchpad.x, event.gtouchpad.y);
                const int vel  = std::clamp(
                    static_cast<int>(event.gtouchpad.pressure * 127.0F), 1, 127);
                if (ctx.drum_pad.finger_note[fi] >= 0)
                    ctx.midi.apply_notes_only(
                        {{Note{.midi_note = ctx.drum_pad.finger_note[fi], .channel = 9}}}, {});
                ctx.drum_pad.finger_note[fi] = note;
                ctx.midi.apply_notes_only(
                    {}, {{Note{.midi_note = note, .velocity = vel, .channel = 9}}});
                ctx.active_notes.apply(
                    MusicalIntent{.note_ons = {{Note{.midi_note = note, .velocity = vel}}}});
                if (ctx.seq.selected_step >= 0) {
                    const auto tidx = static_cast<std::size_t>(ctx.seq.active_track);
                    const auto sidx = static_cast<std::size_t>(ctx.seq.selected_step);
                    auto& track     = ctx.seq.tracks[tidx];
                    track.steps[sidx] = {true, false, 0, vel, note};
                    if (track.midi_bank != ctx.current_midi_bank ||
                        track.midi_program != ctx.current_midi_program ||
                        track.sample_bank >= 0) {
                        track.midi_bank    = ctx.current_midi_bank;
                        track.midi_program = ctx.current_midi_program;
                        track.sample_bank  = -1;
                        ctx.midi.program_change(ctx.current_midi_program, ctx.current_midi_bank,
                                                effective_midi_channel(track));
                    }
                    ctx.seq.selected_step =
                        (ctx.seq.selected_step + 1) % active_track_loop_length(ctx.seq);
                }
            }
        } else if (!l1 && !click && !r1 && fi < ribbon_max) {
            const int note = ribbon_note_for_x(event.gtouchpad.x,
                ctx.last_intent.root_midi_note, ctx.last_intent.octave_offset,
                ctx.last_intent.scale);
            const int vel = std::clamp(
                static_cast<int>((1.0F - event.gtouchpad.y) * 100.0F + 27.0F), 1, 127);
            auto& rf = ctx.ribbon[fi];
            release_ribbon_finger(rf, ctx.midi, ctx.active_notes);
            rf.active         = true;
            rf.midi_note      = note;
            rf.active_channel = ribbon_channel_for_bank(rf, ctx.current_midi_bank);
            ctx.midi.program_change(ctx.current_midi_program, ctx.current_midi_bank,
                                    rf.active_channel);
            ctx.midi.apply_notes_only(
                {}, {{Note{.midi_note = note, .velocity = vel, .channel = rf.active_channel}}});
            ctx.active_notes.apply(
                MusicalIntent{.note_ons = {{Note{.midi_note = note, .velocity = vel}}}});
        }

    } else if (event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION) {
        if (fi == 0 && ctx.tp_swipe.active) {
            constexpr float step_thr  = 0.10F;
            constexpr float track_thr = 0.12F;
            const float dx = event.gtouchpad.x - ctx.tp_swipe.prev_x;
            const float dy = event.gtouchpad.y - ctx.tp_swipe.prev_y;
            if (std::abs(dx) >= std::abs(dy)) {
                const auto length = active_track_loop_length(ctx.seq);
                if (dx > step_thr) {
                    ctx.seq.selected_step = (ctx.seq.selected_step + 1) % length;
                    ctx.tp_swipe.prev_x   = event.gtouchpad.x;
                    ctx.tp_swipe.prev_y   = event.gtouchpad.y;
                } else if (dx < -step_thr) {
                    ctx.seq.selected_step = (ctx.seq.selected_step - 1 + length) % length;
                    ctx.tp_swipe.prev_x   = event.gtouchpad.x;
                    ctx.tp_swipe.prev_y   = event.gtouchpad.y;
                }
            } else {
                if (dy > track_thr) {
                    ctx.seq.active_track = std::min(ctx.seq.active_track + 1,
                        static_cast<int>(ctx.seq.tracks.size()) - 1);
                    ctx.tp_swipe.prev_x = event.gtouchpad.x;
                    ctx.tp_swipe.prev_y = event.gtouchpad.y;
                } else if (dy < -track_thr) {
                    ctx.seq.active_track = std::max(ctx.seq.active_track - 1, 0);
                    ctx.tp_swipe.prev_x  = event.gtouchpad.x;
                    ctx.tp_swipe.prev_y  = event.gtouchpad.y;
                }
            }
        } else if (fi == 0 && ctx.sketch.active) {
            ctx.sketch.points.emplace_back(event.gtouchpad.x, event.gtouchpad.y);
        } else if (fi < ribbon_max && ctx.ribbon[fi].active) {
            const int new_note = ribbon_note_for_x(event.gtouchpad.x,
                ctx.last_intent.root_midi_note, ctx.last_intent.octave_offset,
                ctx.last_intent.scale);
            auto& rf = ctx.ribbon[fi];
            if (new_note != rf.midi_note) {
                const int vel = std::clamp(
                    static_cast<int>((1.0F - event.gtouchpad.y) * 100.0F + 27.0F), 1, 127);
                ctx.midi.apply_notes_only(
                    {{Note{.midi_note = rf.midi_note, .channel = rf.active_channel}}},
                    {{Note{.midi_note = new_note, .velocity = vel,
                           .channel = rf.active_channel}}});
                ctx.active_notes.apply(MusicalIntent{
                    .note_ons  = {{Note{.midi_note = new_note, .velocity = vel}}},
                    .note_offs = {{Note{.midi_note = rf.midi_note}}}});
                rf.midi_note = new_note;
            }
        }

    } else if (event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP) {
        if (fi == 0 && ctx.tp_swipe.active) {
            ctx.tp_swipe.active = false;
        } else if (fi == 0 && ctx.sketch.active) {
            ctx.sketch.active = false;
            auto wavetable = build_wavetable(ctx.sketch.points, wavetable_length);
            if (!wavetable.empty()) {
                ctx.sketch.committed        = build_wavetable(ctx.sketch.points, 128);
                ctx.sketch.committed_points = ctx.sketch.points;
                ctx.audio_sampler.set_wavetable(std::move(wavetable));
                ctx.sampler_mode = true;
                std::cout << "wavetable drawn: " << ctx.sketch.points.size() << " points\n";
            }
            ctx.sketch.points.clear();
        } else if (fi < ribbon_max && ctx.drum_pad.finger_note[fi] >= 0) {
            ctx.midi.apply_notes_only(
                {{Note{.midi_note = ctx.drum_pad.finger_note[fi], .channel = 9}}}, {});
            ctx.active_notes.apply(MusicalIntent{
                .note_offs = {{Note{.midi_note = ctx.drum_pad.finger_note[fi]}}}});
            ctx.drum_pad.finger_note[fi] = -1;
        } else if (fi < ribbon_max && ctx.ribbon[fi].active) {
            release_ribbon_finger(ctx.ribbon[fi], ctx.midi, ctx.active_notes);
        }
    }
}

} // namespace analogno
