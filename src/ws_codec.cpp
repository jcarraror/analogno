#include "ws_codec.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace analogno::ws_codec {

namespace {

using Json    = nlohmann::json;
using Command = WebSocketServer::Command;
using ParseFn = std::function<std::optional<Command>(const Json&)>;

// ── Encode helpers ────────────────────────────────────────────────────────────

Json encode_capture_devices(const std::vector<WebCaptureDevice>& devices) {
    auto arr = Json::array();
    for (const auto& d : devices)
        arr.push_back({{"index", d.index}, {"name", d.name}, {"isDefault", d.is_default}});
    return arr;
}

Json encode_sample_banks(const std::vector<WebSampleBank>& banks) {
    auto arr = Json::array();
    for (const auto& b : banks)
        arr.push_back({
            {"hasData",             b.has_sample},
            {"frames",              b.frames},
            {"trimStart",           b.trim_start},
            {"trimEnd",             b.trim_end},
            {"isWavetable",         b.is_wavetable},
            {"isStream",            b.is_stream},
            {"isLoop",              b.is_loop},
            {"waveform",            b.waveform},
            {"rootNote",            b.root_note},
            {"sliceCount",          b.slice_count},
            {"transcribeCached",    b.transcribe_cached},
            {"seqStored",           b.seq_stored},
            {"generatedTrackCount", b.generated_track_count},
        });
    return arr;
}

// ── Decode helpers for complex commands ───────────────────────────────────────

std::optional<Command> parse_set_seq(const Json& j) {
    using Cfg = WebSocketServer::SeqConfig;
    Cfg cfg{};
    if (j.contains("bpm") && j["bpm"].is_number())
        cfg.bpm = std::clamp(j["bpm"].get<float>(), 20.0F, 300.0F);
    if (j.contains("gatePct") && j["gatePct"].is_number_integer())
        cfg.gate_pct = std::clamp(j["gatePct"].get<int>(), 5, 100);
    if (j.contains("stepCount") && j["stepCount"].is_number_integer()) {
        const auto v = j["stepCount"].get<int>();
        if      (v <= 8)  cfg.step_count = 8;
        else if (v <= 16) cfg.step_count = 16;
        else if (v <= 32) cfg.step_count = 32;
        else              cfg.step_count = Cfg::max_step_count;
    }
    if (j.contains("stepDivision") && j["stepDivision"].is_number_integer()) {
        const auto v = j["stepDivision"].get<int>();
        if      (v <= 8)  cfg.step_division = 8;
        else if (v <= 16) cfg.step_division = 16;
        else              cfg.step_division = 32;
    }
    if (j.contains("tracks") && j["tracks"].is_array()) {
        const auto& tarr = j["tracks"];
        const auto  nt   = std::min(tarr.size(), static_cast<std::size_t>(Cfg::max_tracks));
        cfg.tracks.resize(nt);
        for (std::size_t t = 0; t < nt; ++t) {
            const auto& tj = tarr[t];
            auto&       tr = cfg.tracks[t];
            tr.midi_channel  = std::clamp(tj.value("midiChannel",  -1),  -1, 15);
            tr.midi_program  = std::clamp(tj.value("midiProgram",   0),   0, 127);
            tr.midi_bank     = std::clamp(tj.value("midiBank",      0),   0, 127);
            tr.sample_bank   = std::clamp(tj.value("sampleBank",   -1),  -1, 7);
            tr.loop_length   = std::clamp(tj.value("loopLength", cfg.step_count), 1, cfg.step_count);
            tr.volume        = std::clamp(tj.value("volume",       100),   0, 127);
            tr.pan           = std::clamp(tj.value("pan",           64),   0, 127);
            tr.velocity_scale= std::clamp(tj.value("velocityScale", 100), 50, 200);
            tr.muted         = tj.value("muted", false);
            tr.solo          = tj.value("solo",  false);
            tr.name          = tj.value("name",  std::string{});
            if (tj.contains("steps") && tj["steps"].is_array()) {
                const auto& arr = tj["steps"];
                const auto  n   = std::min(arr.size(), static_cast<std::size_t>(cfg.step_count));
                tr.steps.resize(static_cast<std::size_t>(cfg.step_count));
                for (std::size_t i = 0; i < n; ++i) {
                    const auto& s = arr[i];
                    tr.steps[i] = WebSocketServer::SeqStepConfig{
                        .active      = s.value("active",      false),
                        .tie         = s.value("tie",         false),
                        .degree      = std::clamp(s.value("degree",      0),   0,  27),
                        .velocity    = std::clamp(s.value("velocity",  100),   1, 127),
                        .midi_note   = s.value("midiNote", -1),
                        .probability = std::clamp(s.value("probability", 100), 1, 100),
                    };
                }
            }
        }
    }
    return WebSocketServer::SetSeq{.config = std::move(cfg)};
}

std::optional<Command> parse_set_wavetable(const Json& j) {
    if (!j.contains("data") || !j["data"].is_array()) return std::nullopt;
    std::vector<float> samples;
    samples.reserve(j["data"].size());
    for (const auto& v : j["data"])
        if (v.is_number()) samples.push_back(std::clamp(v.get<float>(), -1.0F, 1.0F));
    if (samples.empty()) return std::nullopt;
    std::vector<float> morph;
    if (j.contains("morphData") && j["morphData"].is_array()) {
        morph.reserve(j["morphData"].size());
        for (const auto& v : j["morphData"])
            if (v.is_number()) morph.push_back(std::clamp(v.get<float>(), -1.0F, 1.0F));
    }
    return WebSocketServer::SetWavetable{
        .wavetable = {.samples = std::move(samples), .morph_samples = std::move(morph)}};
}

std::optional<Command> parse_set_voice_seq(const Json& j) {
    auto mode = j.value("mode", std::string{"percussion"});
    if (mode != "percussion" && mode != "harmonic" && mode != "hybrid")
        mode = "percussion";
    return WebSocketServer::SetVoiceSeq{.config = {
        .enabled          = j.value("enabled",          false),
        .recording        = j.value("recording",        false),
        .mode             = std::move(mode),
        .snap_to_scale    = j.value("snapToScale",       true),
        .sensitivity      = std::clamp(j.value("sensitivity", 65.0F), 0.0F, 100.0F) / 100.0F,
        .timing_offset_ms = std::clamp(j.value("timingOffsetMs", 0.0F), -120.0F, 120.0F),
    }};
}

// ── Command dispatch table ────────────────────────────────────────────────────

const std::unordered_map<std::string, ParseFn>& dispatch_table() {
    static const std::unordered_map<std::string, ParseFn> table = {
        // Zero-argument commands
        {"panic",                [](const Json&) -> std::optional<Command> { return WebSocketServer::Panic{}; }},
        {"seqPlay",              [](const Json&) -> std::optional<Command> { return WebSocketServer::SeqPlay{}; }},
        {"seqStop",              [](const Json&) -> std::optional<Command> { return WebSocketServer::SeqStop{}; }},
        {"seqAddTrack",          [](const Json&) -> std::optional<Command> { return WebSocketServer::SeqAddTrack{}; }},
        {"transcribeMicToSeq",   [](const Json&) -> std::optional<Command> { return WebSocketServer::TranscribeMicToSeq{}; }},
        {"clearAllSeqTracks",    [](const Json&) -> std::optional<Command> { return WebSocketServer::ClearAllSeqTracks{}; }},
        {"removeAllSeqTracks",   [](const Json&) -> std::optional<Command> { return WebSocketServer::RemoveAllSeqTracks{}; }},
        {"openStemFolderDialog", [](const Json&) -> std::optional<Command> { return WebSocketServer::OpenStemFolderDialog{}; }},
        // Complex commands with dedicated parsers
        {"setSeq",               parse_set_seq},
        {"setWavetable",         parse_set_wavetable},
        {"setVoiceSeq",          parse_set_voice_seq},
        // Inline parsers
        {"setCaptureDevice", [](const Json& j) -> std::optional<Command> {
            if (j.contains("deviceIndex") && j["deviceIndex"].is_number_integer())
                return WebSocketServer::SetCaptureDevice{.device_index = j["deviceIndex"].get<int>()};
            return WebSocketServer::SetCaptureDevice{.device_index = std::nullopt};
        }},
        {"setSampleTrim", [](const Json& j) -> std::optional<Command> {
            return WebSocketServer::SetSampleTrim{
                .trim = {.start = j.value("start", 0.0F), .end = j.value("end", 1.0F)}};
        }},
        {"setActiveBank", [](const Json& j) -> std::optional<Command> {
            const auto bank = j.value("bank", -1);
            if (bank < 0) return std::nullopt;
            return WebSocketServer::SetActiveBank{.bank = static_cast<std::size_t>(bank)};
        }},
        {"saveSample", [](const Json& j) -> std::optional<Command> {
            const auto bank = j.value("bank", -1);
            if (bank < 0) return std::nullopt;
            return WebSocketServer::SaveSample{.bank = static_cast<std::size_t>(bank)};
        }},
        {"stemPlay", [](const Json& j) -> std::optional<Command> {
            const auto idx = j.value("idx", -1);
            if (idx < 0) return std::nullopt;
            return WebSocketServer::StemPlay{.idx = static_cast<std::size_t>(idx)};
        }},
        {"stemStop", [](const Json& j) -> std::optional<Command> {
            const auto idx = j.value("idx", -1);
            if (idx < 0) return std::nullopt;
            return WebSocketServer::StemStop{.idx = static_cast<std::size_t>(idx)};
        }},
        {"setStemFolder", [](const Json& j) -> std::optional<Command> {
            const auto path = j.value("path", std::string{});
            if (path.empty()) return std::nullopt;
            return WebSocketServer::SetStemFolder{.path = path};
        }},
        {"setActiveStem", [](const Json& j) -> std::optional<Command> {
            const auto idx = j.value("idx", -1);
            if (idx < 0) return std::nullopt;
            return WebSocketServer::SetActiveStem{.idx = static_cast<std::size_t>(idx)};
        }},
        {"setPatch", [](const Json& j) -> std::optional<Command> {
            const auto bank    = j.value("bank",    0);
            const auto program = j.value("program", 0);
            if (bank < 0 || bank > 128 || program < 0 || program >= 128) return std::nullopt;
            return WebSocketServer::SetPatch{
                .patch = {.bank = bank, .program = static_cast<std::uint8_t>(program)}};
        }},
        {"setSoundfont", [](const Json& j) -> std::optional<Command> {
            if (!j.contains("path") || !j["path"].is_string()) return std::nullopt;
            return WebSocketServer::SetSoundfont{.path = j["path"].get<std::string>()};
        }},
        {"setWavetableControls", [](const Json& j) -> std::optional<Command> {
            return WebSocketServer::SetWavetableControls{.controls = {
                .morph  = std::clamp(j.value("morph",  0.0F), 0.0F, 1.0F),
                .noise  = std::clamp(j.value("noise",  0.0F), 0.0F, 1.0F),
                .unison = std::clamp(j.value("unison", 0.0F), 0.0F, 1.0F),
            }};
        }},
        {"selectSeqStep", [](const Json& j) -> std::optional<Command> {
            return WebSocketServer::SeqSelectStep{.step = std::clamp(j.value("step", -1), -1, 63)};
        }},
        {"selectSeqTrack", [](const Json& j) -> std::optional<Command> {
            return WebSocketServer::SeqSelectTrack{.track = std::clamp(j.value("track", 0), 0, 15)};
        }},
        {"seqRemoveTrack", [](const Json& j) -> std::optional<Command> {
            const auto track = j.value("track", -1);
            if (track < 0) return std::nullopt;
            return WebSocketServer::SeqRemoveTrack{.track = track};
        }},
        {"setTrackVolume", [](const Json& j) -> std::optional<Command> {
            const auto track = j.value("track", -1);
            if (track < 0 || track >= 16) return std::nullopt;
            return WebSocketServer::SetTrackVolume{
                .track = track, .volume = std::clamp(j.value("volume", 100), 0, 127)};
        }},
        {"setTrackPan", [](const Json& j) -> std::optional<Command> {
            const auto track = j.value("track", -1);
            if (track < 0 || track >= 16) return std::nullopt;
            return WebSocketServer::SetTrackPan{
                .track = track, .pan = std::clamp(j.value("pan", 64), 0, 127)};
        }},
        {"setTrackVelocityScale", [](const Json& j) -> std::optional<Command> {
            const auto track = j.value("track", -1);
            if (track < 0 || track >= 16) return std::nullopt;
            return WebSocketServer::SetTrackVelocityScale{
                .track = track, .scale = std::clamp(j.value("scale", 100), 50, 200)};
        }},
        {"setTrackSolo", [](const Json& j) -> std::optional<Command> {
            const auto track = j.value("track", -1);
            if (track < 0 || track >= 16) return std::nullopt;
            return WebSocketServer::SetTrackSolo{.track = track, .solo = j.value("solo", false)};
        }},
        {"setSignalsVolume", [](const Json& j) -> std::optional<Command> {
            return WebSocketServer::SetSignalsVolume{
                .volume = static_cast<float>(std::clamp(j.value("volume", 100), 0, 100)) / 100.0F};
        }},
        {"splitAudioFile", [](const Json& j) -> std::optional<Command> {
            if (!j.contains("path") || !j["path"].is_string()) return std::nullopt;
            return WebSocketServer::SplitAudioFile{.path = j["path"].get<std::string>()};
        }},
        {"streamPlayBank", [](const Json& j) -> std::optional<Command> {
            const auto bank = j.value("bank", -1);
            if (bank < 0) return std::nullopt;
            return WebSocketServer::StreamPlayBank{.bank = static_cast<std::size_t>(bank)};
        }},
        {"streamStopBank", [](const Json& j) -> std::optional<Command> {
            const auto bank = j.value("bank", -1);
            if (bank < 0) return std::nullopt;
            return WebSocketServer::StreamStopBank{.bank = static_cast<std::size_t>(bank)};
        }},
        {"downloadAudio", [](const Json& j) -> std::optional<Command> {
            if (!j.contains("source") || !j["source"].is_string()) return std::nullopt;
            return WebSocketServer::DownloadAudio{.source = j["source"].get<std::string>()};
        }},
        {"loadStemToBank", [](const Json& j) -> std::optional<Command> {
            const auto stem_idx = j.value("stemIdx", -1);
            const auto bank     = j.value("bank",    -1);
            if (stem_idx < 0 || bank < 0) return std::nullopt;
            return WebSocketServer::LoadStemToBank{
                .stem_idx   = static_cast<std::size_t>(stem_idx),
                .bank       = static_cast<std::size_t>(bank),
                .trim_start = std::clamp(j.value("trimStart", 0.0F), 0.0F, 1.0F),
                .trim_end   = std::clamp(j.value("trimEnd",   1.0F), 0.0F, 1.0F),
            };
        }},
        {"setBankRootNote", [](const Json& j) -> std::optional<Command> {
            const auto bank = j.value("bank", -1);
            if (bank < 0 || bank >= 8) return std::nullopt;
            return WebSocketServer::SetBankRootNote{
                .bank = bank, .note = std::clamp(j.value("note", 48), 0, 127)};
        }},
        {"setBankSliceCount", [](const Json& j) -> std::optional<Command> {
            const auto bank = j.value("bank", -1);
            if (bank < 0 || bank >= 8) return std::nullopt;
            return WebSocketServer::SetBankSliceCount{
                .bank = bank, .count = std::clamp(j.value("count", 0), 0, 64)};
        }},
        {"transcribeBankToSeq", [](const Json& j) -> std::optional<Command> {
            const auto bank = j.value("bank", -1);
            if (bank < 0 || bank >= 8) return std::nullopt;
            return WebSocketServer::TranscribeBankToSeq{.bank = bank};
        }},
        {"revertToTranscribed", [](const Json& j) -> std::optional<Command> {
            const auto bank = j.value("bank", -1);
            if (bank < 0 || bank >= 8) return std::nullopt;
            return WebSocketServer::RevertToTranscribed{.bank = bank};
        }},
        {"loadBankSeq", [](const Json& j) -> std::optional<Command> {
            const auto bank = j.value("bank", -1);
            if (bank < 0 || bank >= 8) return std::nullopt;
            return WebSocketServer::LoadBankSeq{.bank = bank};
        }},
        {"arrangeBankToSeq", [](const Json& j) -> std::optional<Command> {
            const auto bank = j.value("bank", -1);
            if (bank < 0 || bank >= 8) return std::nullopt;
            return WebSocketServer::ArrangeBankToSeq{.bank = bank};
        }},
        {"clearSeqTrack", [](const Json& j) -> std::optional<Command> {
            const auto track = j.value("track", -1);
            if (track < 0) return std::nullopt;
            return WebSocketServer::ClearSeqTrack{.track = track};
        }},
        {"setSeqTrackName", [](const Json& j) -> std::optional<Command> {
            const auto track = j.value("track", -1);
            if (track < 0) return std::nullopt;
            return WebSocketServer::SetSeqTrackName{
                .track = track, .name = j.value("name", std::string{})};
        }},
        {"setSeqSwing", [](const Json& j) -> std::optional<Command> {
            return WebSocketServer::SetSeqSwing{.swing = std::clamp(j.value("swing", 0), 0, 50)};
        }},
        {"setStepProbability", [](const Json& j) -> std::optional<Command> {
            const auto track = j.value("track", -1);
            const auto step  = j.value("step",  -1);
            if (track < 0 || step < 0) return std::nullopt;
            return WebSocketServer::SetStepProbability{
                .track = track, .step = step,
                .probability = std::clamp(j.value("probability", 100), 1, 100)};
        }},
        {"copySeqTrack", [](const Json& j) -> std::optional<Command> {
            const auto track = j.value("track", -1);
            if (track < 0) return std::nullopt;
            return WebSocketServer::CopySeqTrack{.track = track};
        }},
        {"pasteSeqTrack", [](const Json& j) -> std::optional<Command> {
            const auto track = j.value("track", -1);
            if (track < 0) return std::nullopt;
            return WebSocketServer::PasteSeqTrack{.track = track};
        }},
        {"setBankStreamLoop", [](const Json& j) -> std::optional<Command> {
            const auto bank = j.value("bank", -1);
            if (bank < 0 || bank >= 8) return std::nullopt;
            return WebSocketServer::SetBankStreamLoop{
                .bank = static_cast<std::size_t>(bank),
                .loop = j.value("loop", false)};
        }},
        {"setBankTrim", [](const Json& j) -> std::optional<Command> {
            const auto bank = j.value("bank", -1);
            if (bank < 0 || bank >= 8) return std::nullopt;
            return WebSocketServer::SetBankTrim{
                .bank  = static_cast<std::size_t>(bank),
                .start = std::clamp(j.value("start", 0.0F), 0.0F, 1.0F),
                .end   = std::clamp(j.value("end",   1.0F), 0.0F, 1.0F)};
        }},
        {"scrubStream", [](const Json& j) -> std::optional<Command> {
            const auto bank = j.value("bank", -1);
            if (bank < 0 || bank >= 8) return std::nullopt;
            return WebSocketServer::ScrubStream{
                .bank = static_cast<std::size_t>(bank),
                .frac = std::clamp(j.value("frac", 0.0F), 0.0F, 1.0F)};
        }},
    };
    return table;
}

} // namespace

// ── Public encode API ─────────────────────────────────────────────────────────

std::string encode(const WebTickState& state) {
    const Json json{
        {"type", "tick"},
        {"controller", {
            {"leftX",    state.controller.left_x},
            {"leftY",    state.controller.left_y},
            {"rightX",   state.controller.right_x},
            {"rightY",   state.controller.right_y},
            {"l2",       state.controller.left_trigger},
            {"r2",       state.controller.right_trigger},
            {"hasGyro",  state.controller.has_gyro},
            {"hasAccel", state.controller.has_accel},
            {"gyro",  {{"x", state.controller.gyro.x},  {"y", state.controller.gyro.y},  {"z", state.controller.gyro.z}}},
            {"accel", {{"x", state.controller.accel.x}, {"y", state.controller.accel.y}, {"z", state.controller.accel.z}}},
        }},
        {"music", {
            {"pitchBend",       state.music.pitch_bend},
            {"expression",      state.music.expression},
            {"filterCutoff",    state.music.filter_cutoff},
            {"filterResonance", state.music.filter_resonance},
            {"modulation",      state.music.modulation},
            {"vibrato",         state.music.vibrato},
            {"activeNotes",     state.music.active_notes},
        }},
        {"audio", {
            {"micLevel",                 state.audio.mic_level},
            {"envelope",                 state.audio.envelope},
            {"gateOpen",                 state.audio.gate_open},
            {"onset",                    state.audio.onset},
            {"velocity",                 state.audio.velocity},
            {"waveform",                 state.audio.waveform},
            {"specSamples",              state.audio.spec_samples},
            {"touchpadSketch",           state.audio.touchpad_sketch},
            {"touchpadRawPoints",        state.audio.touchpad_raw_points},
            {"voiceSeqRecording",        state.audio.voice_seq_recording},
            {"voiceSeqRecordProgress",   state.audio.voice_seq_record_progress},
            {"voiceSeqLastNote",         state.audio.voice_seq_last_note},
            {"voiceSeqLastVelocity",     state.audio.voice_seq_last_velocity},
            {"voiceSeqAcceptedNotes",    state.audio.voice_seq_accepted_notes},
            {"voiceSeqRejectedNotes",    state.audio.voice_seq_rejected_notes},
            {"voiceSeqRecordedSegments", state.audio.voice_seq_recorded_segments},
            {"stemSplitState",           state.audio.stem_split_state},
            {"stemSplitProgress",        state.audio.stem_split_progress},
            {"stemSplitDetail",          state.audio.stem_split_detail},
        }},
        {"seq", {
            {"playing",      state.seq.playing},
            {"playheadStep", state.seq.playhead_step},
            {"currentStep",  state.seq.current_step},
        }},
    };
    return json.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string encode(const WebRuntimeState& state) {
    auto tracks_json = Json::array();
    for (const auto& track : state.seq.tracks) {
        auto steps_json = Json::array();
        for (const auto& s : track.steps)
            steps_json.push_back({
                {"active",      s.active},
                {"tie",         s.tie},
                {"degree",      s.degree},
                {"velocity",    s.velocity},
                {"midiNote",    s.midi_note},
                {"probability", s.probability},
            });
        tracks_json.push_back({
            {"midiChannel",   track.midi_channel},
            {"midiProgram",   track.midi_program},
            {"midiBank",      track.midi_bank},
            {"sampleBank",    track.sample_bank},
            {"loopLength",    track.loop_length},
            {"volume",        track.volume},
            {"pan",           track.pan},
            {"velocityScale", track.velocity_scale},
            {"muted",         track.muted},
            {"solo",          track.solo},
            {"name",          track.name},
            {"steps",         std::move(steps_json)},
        });
    }

    auto stems_json = Json::array();
    for (const auto& s : state.audio.stems)
        stems_json.push_back({
            {"name",     s.name},
            {"frames",   s.frames},
            {"isActive", s.is_active},
            {"waveform", s.waveform},
        });

    const Json json{
        {"type", "runtime"},
        {"controller", {
            {"leftX",    state.controller.left_x},
            {"leftY",    state.controller.left_y},
            {"rightX",   state.controller.right_x},
            {"rightY",   state.controller.right_y},
            {"l2",       state.controller.left_trigger},
            {"r2",       state.controller.right_trigger},
            {"hasGyro",  state.controller.has_gyro},
            {"hasAccel", state.controller.has_accel},
            {"gyro",  {{"x", state.controller.gyro.x},  {"y", state.controller.gyro.y},  {"z", state.controller.gyro.z}}},
            {"accel", {{"x", state.controller.accel.x}, {"y", state.controller.accel.y}, {"z", state.controller.accel.z}}},
        }},
        {"music", {
            {"rootMidiNote",    state.music.root_midi_note},
            {"octaveOffset",    state.music.octave_offset},
            {"scale",           state.music.scale},
            {"pitchBend",       state.music.pitch_bend},
            {"expression",      state.music.expression},
            {"filterCutoff",    state.music.filter_cutoff},
            {"filterResonance", state.music.filter_resonance},
            {"modulation",      state.music.modulation},
            {"vibrato",         state.music.vibrato},
            {"activeNotes",     state.music.active_notes},
            {"midiProgram",     state.music.midi_program},
            {"midiBank",        state.music.midi_bank},
            {"buttonMidiNotes", state.music.button_midi_notes},
        }},
        {"audio", {
            {"devices",                  encode_capture_devices(state.audio.devices)},
            {"selectedDeviceIndex",      state.audio.selected_device_index},
            {"captureRunning",           state.audio.capture_running},
            {"sampleRecording",          state.audio.sample_recording},
            {"captureDevice",            state.audio.capture_device},
            {"micLevel",                 state.audio.mic_level},
            {"envelope",                 state.audio.envelope},
            {"gateOpen",                 state.audio.gate_open},
            {"onset",                    state.audio.onset},
            {"velocity",                 state.audio.velocity},
            {"waveform",                 state.audio.waveform},
            {"sampleReady",              state.audio.sample_ready},
            {"sampleFrames",             state.audio.sample_frames},
            {"sampleTrimStart",          state.audio.sample_trim_start},
            {"sampleTrimEnd",            state.audio.sample_trim_end},
            {"sampleWaveform",           state.audio.sample_waveform},
            {"wavetableCycle",           state.audio.wavetable_cycle},
            {"banks",                    encode_sample_banks(state.audio.banks)},
            {"activeBank",               state.audio.active_bank},
            {"touchpadDrawing",          state.audio.touchpad_drawing},
            {"touchpadSketch",           state.audio.touchpad_sketch},
            {"touchpadRawPoints",        state.audio.touchpad_raw_points},
            {"signalsVolume",            state.audio.signals_volume},
            {"wavetableMorph",           state.audio.wavetable_morph},
            {"wavetableNoise",           state.audio.wavetable_noise},
            {"wavetableUnison",          state.audio.wavetable_unison},
            {"voiceSeqAvailable",        state.audio.voice_seq_available},
            {"voiceSeqCompiled",         state.audio.voice_seq_compiled},
            {"voiceSeqEnabled",          state.audio.voice_seq_enabled},
            {"voiceSeqRecording",        state.audio.voice_seq_recording},
            {"voiceSeqMode",             state.audio.voice_seq_mode},
            {"voiceSeqSnap",             state.audio.voice_seq_snap},
            {"voiceSeqSensitivity",      state.audio.voice_seq_sensitivity},
            {"voiceSeqTimingOffsetMs",   state.audio.voice_seq_timing_offset_ms},
            {"voiceSeqLastNote",         state.audio.voice_seq_last_note},
            {"voiceSeqLastVelocity",     state.audio.voice_seq_last_velocity},
            {"voiceSeqAcceptedNotes",    state.audio.voice_seq_accepted_notes},
            {"voiceSeqRejectedNotes",    state.audio.voice_seq_rejected_notes},
            {"voiceSeqRecordedSegments", state.audio.voice_seq_recorded_segments},
            {"voiceSeqRecordProgress",   state.audio.voice_seq_record_progress},
            {"specSamples",              state.audio.spec_samples},
            {"micHasSample",             state.audio.mic_has_sample},
            {"transcribeState",          state.audio.transcribe_state},
            {"stemSplitState",           state.audio.stem_split_state},
            {"stemSplitError",           state.audio.stem_split_error},
            {"stemSplitProgress",        state.audio.stem_split_progress},
            {"stemSplitDetail",          state.audio.stem_split_detail},
            {"stemSplitLog",             state.audio.stem_split_log},
            {"stemsFolder",              state.audio.stems_folder},
            {"stems",                    std::move(stems_json)},
        }},
        {"seq", {
            {"playing",            state.seq.playing},
            {"activeTrack",        state.seq.active_track},
            {"selectedStep",       state.seq.selected_step},
            {"bpm",                state.seq.bpm},
            {"playheadStep",       state.seq.playhead_step},
            {"currentStep",        state.seq.current_step},
            {"gatePct",            state.seq.gate_pct},
            {"stepCount",          state.seq.step_count},
            {"stepDivision",       state.seq.step_division},
            {"swing",              state.seq.swing},
            {"clipboardAvailable", state.seq.clipboard_available},
            {"tracks",             std::move(tracks_json)},
        }},
        {"pianoRollVisible",   state.piano_roll_visible},
        {"spectrogramVisible", state.spectrogram_visible},
    };
    return json.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string encode(const WebLibraryState& state) {
    auto presets_json = Json::array();
    for (const auto& p : state.presets)
        presets_json.push_back({{"bank", p.bank}, {"program", p.program}, {"name", p.name}});

    auto soundfonts_json = Json::array();
    for (const auto& s : state.soundfonts)
        soundfonts_json.push_back(s);

    const Json json{
        {"type",           "library"},
        {"presets",        std::move(presets_json)},
        {"soundfonts",     std::move(soundfonts_json)},
        {"activeSoundfont", state.active_soundfont},
    };
    return json.dump(-1, ' ', false, Json::error_handler_t::replace);
}

// ── Public decode API ─────────────────────────────────────────────────────────

std::optional<WebSocketServer::Command> decode(std::string_view message) {
    try {
        const auto json = Json::parse(message);
        const auto& table = dispatch_table();
        const auto  it    = table.find(json.value("type", ""));
        if (it == table.end()) return std::nullopt;
        return it->second(json);
    } catch (const std::exception& e) {
        std::cerr << "invalid websocket JSON: " << e.what() << '\n';
        return std::nullopt;
    }
}

} // namespace analogno::ws_codec
