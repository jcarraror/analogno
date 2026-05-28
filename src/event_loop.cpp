#include "event_loop.hpp"

#include "app_context.hpp"
#include "command_handler.hpp"
#include "led_colors.hpp"
#include "live_router.hpp"
#include "runtime_state_builder.hpp"
#include "sdl_check.hpp"
#include "sequencer.hpp"
#include "session.hpp"
#include "stem_pipeline.hpp"
#include "touchpad_handler.hpp"
#include "transcribe_manager.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace analogno {

namespace {

static std::atomic<bool> s_quit{false};

// ── SDL event dispatch ────────────────────────────────────────────────────────

[[nodiscard]] bool handle_sdl_event(const SDL_Event& event, ControllerState& state) {
    switch (event.type) {
    case SDL_EVENT_QUIT: return false;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:     state.handle_button_down(event.gbutton);     break;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:       state.handle_button_up(event.gbutton);       break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:     state.handle_axis(event.gaxis);              break;
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:   state.handle_sensor(event.gsensor);          break;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:   state.handle_touchpad_down(event.gtouchpad); break;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION: state.handle_touchpad_motion(event.gtouchpad); break;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:     state.handle_touchpad_up(event.gtouchpad);   break;
    default: break;
    }
    return true;
}

// ── Sampler gain ──────────────────────────────────────────────────────────────

void update_sampler_controls(const ContinuousControls& controls, AudioSampler& sampler,
                              bool seq_playing, bool l2_controls_gain, float signals_volume) {
    constexpr float gain_boost = 1.25F;
    const auto live_gain = l2_controls_gain ? controls.expression : 1.0F;
    const auto gain      = (seq_playing ? 1.0F : live_gain) * gain_boost * signals_volume;
    sampler.set_gain(gain);
    sampler.set_pitch_controls(controls.pitch_bend, controls.vibrato);
}

[[nodiscard]] bool compute_l2_controls_gain(const AppContext& ctx) {
    const auto asb = ctx.audio_sampler.active_bank();
    const auto at  = ctx.seq.active_track;
    const int tsb  = (at >= 0 && static_cast<std::size_t>(at) < ctx.seq.tracks.size())
                     ? ctx.seq.tracks[static_cast<std::size_t>(at)].sample_bank
                     : -1;
    return (tsb >= 0 &&
            ctx.audio_sampler.bank_has_sample(static_cast<std::size_t>(tsb)) &&
            !ctx.audio_sampler.bank_is_wavetable(static_cast<std::size_t>(tsb)) &&
            !ctx.audio_sampler.bank_is_stream(static_cast<std::size_t>(tsb))) ||
           (ctx.sampler_mode && ctx.audio_sampler.has_sample() &&
            !ctx.audio_sampler.bank_is_wavetable(asb) &&
            !ctx.audio_sampler.bank_is_stream(asb));
}


// ── Per-frame step handlers ───────────────────────────────────────────────────

void handle_buttons(AppContext& ctx, bool& piano_roll_visible, bool& spectrogram_visible,
                    std::chrono::steady_clock::time_point& clock_next) {
    auto& c  = ctx.controller;
    auto& as = ctx.audio_sampler;

    if (c.button_pressed(SDL_GAMEPAD_BUTTON_TOUCHPAD) &&
        !c.button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) {
        const auto next = (as.active_bank() + 1) % AudioSampler::bank_count;
        as.stop_all();
        as.set_active_bank(next);
        ctx.sampler_mode = as.has_sample();
        std::cout << "bank: " << next << '\n';
    }

    if (c.button_pressed(SDL_GAMEPAD_BUTTON_BACK)) {
        const auto cur  = as.active_bank();
        const auto prev = (cur == 0 ? AudioSampler::bank_count : cur) - 1;
        as.stop_all();
        as.set_active_bank(prev);
        ctx.sampler_mode = as.has_sample();
        std::cout << "bank: " << prev << '\n';
    }

    if (c.button_pressed(SDL_GAMEPAD_BUTTON_RIGHT_STICK)) {
        piano_roll_visible = !piano_roll_visible;
        std::cout << "piano roll: " << (piano_roll_visible ? "on" : "off") << '\n';
    }

    if (c.button_pressed(SDL_GAMEPAD_BUTTON_LEFT_STICK)) {
        spectrogram_visible = !spectrogram_visible;
        std::cout << "spectrogram: " << (spectrogram_visible ? "on" : "off") << '\n';
    }

    if (c.button_pressed(SDL_GAMEPAD_BUTTON_START)) {
        if (ctx.seq.playing) {
            ctx.seq.playing = false;
            for (auto& track : ctx.seq.tracks) {
                if (track.pending_note_off) {
                    ctx.midi.apply_notes_only({*track.pending_note_off}, {});
                    track.pending_note_off.reset();
                }
            }
            ctx.midi.send_stop();
            save_session(ctx.seq, ctx.stems_folder, ctx.session_path);
            std::cout << "seq: stop (controller)\n";
        } else {
            ctx.seq.playing       = true;
            ctx.seq.current_step  = -1;
            ctx.seq.playhead_step = -1;
            ctx.seq.step_start    = std::chrono::steady_clock::now();
            ctx.midi.send_start();
            clock_next = std::chrono::steady_clock::now();
            std::cout << "seq: play (controller)\n";
        }
    }

    if (as.has_sample() && c.button_pressed(SDL_GAMEPAD_BUTTON_GUIDE)) {
        as.clear_sample();
        ctx.sampler_mode = false;
        MusicalIntent panic{};
        panic.note_off_all = true;
        ctx.midi.apply(panic);
        as.stop_all();
        as.stream_stop_all();
        ctx.active_notes.apply(panic);
        ctx.last_intent = panic;
        std::cout << "sample cleared\n";
    }
}

void handle_mic_recording(AppContext& ctx, bool& was_active) {
    const bool active =
        ctx.controller.button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) &&
        ctx.controller.button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    if (active && !was_active) {
        ctx.audio_capture.begin_sample_recording();
        std::cout << "sampler recording started\n";
    }
    if (!active && was_active) {
        ctx.audio_capture.end_sample_recording();
        std::cout << "sampler recording stopped\n";
    }
    was_active = active;

    if (auto sample = ctx.audio_capture.consume_captured_sample()) {
        const auto frame_count = sample->size();
        ctx.transcribe.set_mic_capture(*sample);
        ctx.transcribe.invalidate_cache(ctx.audio_sampler.active_bank());
        ctx.audio_sampler.set_sample(std::move(*sample));
        ctx.sampler_mode = true;
        MusicalIntent panic{};
        panic.note_off_all = true;
        ctx.midi.apply(panic);
        ctx.audio_sampler.stop_all();
        ctx.active_notes.apply(panic);
        ctx.last_intent = panic;
        std::cout << "captured: " << frame_count << " frames\n";
        if (ctx.seq.active_track >= 0 &&
            static_cast<std::size_t>(ctx.seq.active_track) < ctx.seq.tracks.size()) {
            ctx.transcribe.kick_mic(
                ctx.audio_sampler.active_bank(),
                static_cast<std::size_t>(ctx.seq.active_track),
                {.bpm           = static_cast<float>(ctx.seq.bpm),
                 .step_division = ctx.seq.step_division,
                 .step_count    = ctx.seq.step_count});
        }
    }
}

void tick_seq(AppContext& ctx) {
    const auto tick = tick_sequencer(ctx.seq, ctx.last_intent);
    if (!tick.note_ons.empty() || !tick.note_offs.empty())
        ctx.midi.apply_notes_only(tick.note_offs, tick.note_ons);

    for (const auto& ev : tick.sample_note_offs) {
        const auto bank = static_cast<std::size_t>(ev.bank);
        if (ctx.audio_sampler.bank_has_onsets(bank))
            ctx.audio_sampler.release_bank_onset(bank, ev.note.degree);
        else
            ctx.audio_sampler.release_bank(bank,
                std::pow(2.0F, static_cast<float>(
                    ev.note.midi_note - ctx.audio_sampler.bank_root_note(bank)) / 12.0F),
                ev.note.midi_note);
    }
    for (const auto& ev : tick.sample_note_ons) {
        const auto bank = static_cast<std::size_t>(ev.bank);
        if (ctx.audio_sampler.bank_has_onsets(bank))
            ctx.audio_sampler.trigger_bank_onset(bank, ev.note.degree);
        else
            ctx.audio_sampler.trigger_bank(bank,
                std::pow(2.0F, static_cast<float>(
                    ev.note.midi_note - ctx.audio_sampler.bank_root_note(bank)) / 12.0F),
                ev.note.midi_note);
    }

    if (!tick.note_ons.empty()) {
        std::array<int, 16> ch_prog{};
        for (const auto& track : ctx.seq.tracks) {
            const auto ch = std::clamp(effective_midi_channel(track), 0, 15);
            ch_prog[static_cast<std::size_t>(ch)] = track.midi_program;
        }
        constexpr auto led_slot = std::chrono::milliseconds{80};
        for (const auto& note : tick.note_ons) {
            const auto ch        = std::clamp(note.channel, 0, 15);
            const auto starts_at = ctx.led_queue.empty()
                ? std::chrono::steady_clock::now()
                : ctx.led_queue.back().show_until;
            ctx.led_queue.push_back({
                sequencer_led_color(ch_prog[static_cast<std::size_t>(ch)]),
                starts_at + led_slot});
        }
    }
}

void tick_midi_clock(Sequencer& seq, MidiOutput& midi,
                     std::chrono::steady_clock::time_point& clock_next) {
    if (!seq.playing) return;
    const auto now = std::chrono::steady_clock::now();
    using clock_ms = std::chrono::duration<double, std::milli>;
    const clock_ms tick_ms{60000.0 / (static_cast<double>(seq.bpm) * 24.0)};
    while (now >= clock_next) {
        midi.send_clock();
        clock_next += std::chrono::round<std::chrono::steady_clock::duration>(tick_ms);
    }
}

void update_lightbar(Gamepad& gamepad, AppContext& ctx) {
    const auto now = std::chrono::steady_clock::now();
    if (now < ctx.drum_pad.flash_until) {
        const auto c = ctx.drum_pad.page_color();
        gamepad.set_led(c[0], c[1], c[2]);
    } else if (ctx.seq.playing) {
        while (!ctx.led_queue.empty() && now >= ctx.led_queue.front().show_until)
            ctx.led_queue.pop_front();
        if (!ctx.led_queue.empty()) {
            const auto& f = ctx.led_queue.front();
            gamepad.set_led(f.color[0], f.color[1], f.color[2]);
        } else {
            gamepad.set_led(10, 10, 14);
        }
    } else {
        ctx.led_queue.clear();
        gamepad.set_led(0, 0, 0);
    }
}

void publish_state(AppContext& ctx, bool piano_roll_visible, bool spectrogram_visible,
                   int& spec_counter, std::chrono::steady_clock::time_point& last_publish) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_publish < std::chrono::milliseconds{100}) return;

    const auto spec_data =
        (spectrogram_visible && (++spec_counter % 3 == 0))
        ? ctx.audio_capture.spec_samples()
        : std::vector<float>{};

    const auto& stem_st         = ctx.stem_pipeline.status();
    const auto active_notes_vec = ctx.active_notes.notes();
    const auto voice_status     = ctx.voice_seq.status();
    const std::string transcribe_state =
        ctx.transcribe.is_running() ? "running" : "idle";

    ctx.web.publish_runtime(make_web_state(WebStateParams{
        .controller          = ctx.controller,
        .intent              = ctx.last_intent,
        .active_notes        = active_notes_vec,
        .audio_capture       = ctx.audio_capture,
        .audio_sampler       = ctx.audio_sampler,
        .audio_features      = ctx.last_audio_features,
        .midi_program        = ctx.current_midi_program,
        .midi_bank           = ctx.current_midi_bank,
        .sketch              = ctx.sketch,
        .seq                 = ctx.seq,
        .piano_roll_visible  = piano_roll_visible,
        .spectrogram_visible = spectrogram_visible,
        .wavetable_morph     = ctx.wavetable_morph,
        .wavetable_noise     = ctx.wavetable_noise,
        .wavetable_unison    = ctx.wavetable_unison,
        .voice_seq_status    = voice_status,
        .spec_samples        = spec_data,
        .signals_volume      = ctx.signals_volume,
        .stem_state          = stem_st.state,
        .stem_error          = stem_st.error,
        .stem_progress       = stem_st.progress,
        .stem_detail         = stem_st.detail,
        .stem_log            = stem_st.log,
        .stems_folder        = ctx.stems_folder,
        .transcribe_state    = transcribe_state,
        .mic_has_sample      = ctx.transcribe.has_mic_capture(),
        .transcribe_cached   = ctx.transcribe.cache_mask(),
    }));
    last_publish = now;
}

void drain_rumble(Gamepad& gamepad, DrumPad& drum_pad) {
    const auto now = std::chrono::steady_clock::now();
    while (!drum_pad.pending_rumble.empty() &&
           now >= drum_pad.pending_rumble.front().fire_at) {
        const auto& ev = drum_pad.pending_rumble.front();
        gamepad.rumble(ev.low, ev.high, ev.ms);
        drum_pad.pending_rumble.pop_front();
    }
}

} // namespace

// ── Public API ────────────────────────────────────────────────────────────────

Sdl::Sdl() {
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        fail_sdl("SDL_Init failed");
}

Sdl::~Sdl() { SDL_Quit(); }

void signal_quit() noexcept { s_quit = true; }

std::vector<std::string> scan_soundfonts() {
    namespace fs = std::filesystem;
    std::vector<std::string> result;
    std::vector<std::string> dirs = {
        (fs::current_path() / "soundfonts").string(),
        "/usr/share/sounds/sf2",
        "/usr/share/soundfonts",
    };
    if (const char* home = std::getenv("HOME"))
        dirs.push_back(std::string{home} + "/.local/share/sounds/sf2");
    for (const auto& dir : dirs) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.path().extension() == ".sf2")
                result.push_back(e.path().string());
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::optional<Gamepad> open_first_gamepad() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids) fail_sdl("SDL_GetGamepads failed");
    std::unique_ptr<SDL_JoystickID[], decltype(&SDL_free)> owner{ids, SDL_free};
    if (count == 0) { std::cout << "no gamepads found\n"; return std::nullopt; }
    std::cout << "gamepads: " << count << '\n';
    const std::span<const SDL_JoystickID> all{ids, static_cast<std::size_t>(count)};
    for (const SDL_JoystickID id : all) {
        const char* name = SDL_GetGamepadNameForID(id);
        std::cout << "  id=" << id << " name=" << (name ? name : "?") << '\n';
    }
    return Gamepad{all.front()};
}

void run_event_loop(Gamepad& gamepad, std::vector<WebPreset> sf2_presets,
                    std::string active_soundfont,
                    const std::vector<std::string>& available_soundfonts) {
    std::cout << "analogno running — Ctrl+C to quit\n\n";

    // === Subsystems ===
    ControllerState controller{};
    MusicMapper mapper{};
    MidiOutput midi{};
    ActiveNoteTracker active_notes{};
    AudioCapture audio_capture{};
    audio_capture.start();
    audio_capture.start_loopback();
    AudioSampler audio_sampler{audio_capture.shared_context()};
    WebSocketServer web{};
    web.start();
    web.publish_library(make_web_library_state(sf2_presets, available_soundfonts, active_soundfont));
    VoiceSequencer voice_seq{};
    DrumPad drum_pad{};
    TranscribeManager transcribe{};
    StemPipeline stem_pipeline{};

    // === Session load ===
    const auto session_path  = default_session_path();
    const std::string stems_base_dir{"/tmp/analogno-stems"};
    Sequencer seq{};
    std::string stems_folder = stems_base_dir;
    auto current_midi_bank    = int{0};
    auto current_midi_program = int{0};

    if (auto loaded = load_session(session_path)) {
        seq = std::move(loaded->seq);
        if (!loaded->stems_folder.empty() &&
            std::filesystem::exists(loaded->stems_folder))
            stems_folder = loaded->stems_folder;
        for (const auto& track : seq.tracks) {
            const auto ch = effective_midi_channel(track);
            midi.set_channel_volume(ch, track.volume);
            midi.set_channel_pan(ch, track.pan);
            if (track.sample_bank < 0)
                midi.program_change(track.midi_program, track.midi_bank, ch);
        }
    }
    {
        const auto at = static_cast<std::size_t>(seq.active_track);
        if (at < seq.tracks.size()) {
            current_midi_bank    = seq.tracks[at].midi_bank;
            current_midi_program = seq.tracks[at].midi_program;
        }
    }

    // === Frame state ===
    bool running                  = true;
    bool sampler_mode             = false;
    bool piano_roll_visible       = true;
    bool spectrogram_visible      = true;
    float signals_volume          = 1.0F;
    float wavetable_morph         = 0.0F;
    float wavetable_noise         = 0.0F;
    float wavetable_unison        = 0.0F;
    bool was_sample_record_active = false;
    MusicalIntent last_intent{};
    AudioFeatures last_audio_features{};
    WaveformSketch sketch{};
    std::array<RibbonFinger, 2> ribbon{};
    ribbon[0].midi_channel = 12;
    ribbon[1].midi_channel = 13;
    TouchSwipe tp_swipe{};
    std::deque<LedFlash> led_queue{};
    auto last_web_publish = std::chrono::steady_clock::time_point{};
    auto clock_next       = std::chrono::steady_clock::time_point{};
    int spec_counter      = 0;

    // === AppContext ===
    AppContext ctx{
        .controller           = controller,
        .mapper               = mapper,
        .midi                 = midi,
        .active_notes         = active_notes,
        .audio_capture        = audio_capture,
        .audio_sampler        = audio_sampler,
        .web                  = web,
        .voice_seq            = voice_seq,
        .drum_pad             = drum_pad,
        .seq                  = seq,
        .sketch               = sketch,
        .transcribe           = transcribe,
        .stem_pipeline        = stem_pipeline,
        .sampler_mode         = sampler_mode,
        .current_midi_bank    = current_midi_bank,
        .current_midi_program = current_midi_program,
        .signals_volume       = signals_volume,
        .wavetable_morph      = wavetable_morph,
        .wavetable_noise      = wavetable_noise,
        .wavetable_unison     = wavetable_unison,
        .stems_folder         = stems_folder,
        .stems_base_dir       = stems_base_dir,
        .last_intent          = last_intent,
        .last_audio_features  = last_audio_features,
        .session_path         = session_path,
        .sf2_presets          = sf2_presets,
        .active_soundfont     = active_soundfont,
        .available_soundfonts = available_soundfonts,
        .ribbon               = ribbon,
        .tp_swipe             = tp_swipe,
        .led_queue            = led_queue,
    };

    const auto load_stems_fn = [&](const std::string& folder) {
        return load_stems(folder, audio_sampler);
    };

    web.set_stems_folder(stems_folder);
    if (load_stems_fn(stems_folder) > 0)
        stem_pipeline.mark_stems_ready();

    // =========================================================================
    // Main loop
    // =========================================================================
    while (running) {
        if (s_quit) running = false;

        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            running = handle_sdl_event(event, controller);
            if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                event.gbutton.button == SDL_GAMEPAD_BUTTON_TOUCHPAD &&
                seq.selected_step >= 0) {
                const auto tidx = static_cast<std::size_t>(seq.active_track);
                const auto sidx = static_cast<std::size_t>(seq.selected_step);
                if (tidx < seq.tracks.size()) {
                    seq.tracks[tidx].steps[sidx] = {};
                    std::cout << "seq: cleared step " << seq.selected_step
                              << " track " << seq.active_track << '\n';
                }
            }
            handle_touchpad_event(event, ctx);
        }

        dispatch_web_commands(ctx, load_stems_fn);

        if (auto new_folder = stem_pipeline.tick(stems_base_dir)) {
            stems_folder = *new_folder;
            web.set_stems_folder(stems_folder);
            load_stems_fn(stems_folder);
        }

        if (auto job = transcribe.poll()) {
            if (!job->result.tracks.empty() && !job->result.onset_frames.empty()) {
                transcribe.store_cache(job->bank_idx, job->result);
                apply_transcription(job->result, job->bank_idx, job->first_track,
                                    audio_sampler, seq);
            } else {
                std::cout << "[transcribe] no onsets detected\n";
            }
        }

        handle_buttons(ctx, piano_roll_visible, spectrogram_visible, clock_next);
        handle_mic_recording(ctx, was_sample_record_active);

        const auto audio_features = audio_capture.consume_features();
        update_sampler_controls(mapper.map_controls(controller), audio_sampler,
                                seq.playing, compute_l2_controls_gain(ctx), signals_volume);

        if (audio_capture.is_sample_recording()) {
            static_cast<void>(audio_capture.consume_analysis_frames(8192));
        } else {
            voice_seq.process(audio_capture, last_intent.root_midi_note,
                              last_intent.octave_offset, last_intent.scale);
        }

        route_live_notes(ctx, audio_features);
        tick_seq(ctx);
        tick_midi_clock(seq, midi, clock_next);
        update_lightbar(gamepad, ctx);
        publish_state(ctx, piano_roll_visible, spectrogram_visible,
                      spec_counter, last_web_publish);
        drain_rumble(gamepad, drum_pad);

        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    if (seq.playing) midi.send_stop();
    save_session(seq, stems_folder, session_path);
}

} // namespace analogno
