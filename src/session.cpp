#include "session.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace analogno {

namespace {
using Json = nlohmann::json;
}

std::string default_session_path() {
  const auto *home = std::getenv("HOME");
  return std::string{home ? home : "/tmp"} +
         "/.local/share/analogno/session.json";
}

void save_session(const Sequencer &seq, const std::string &path) {
  Json jtracks = Json::array();
  for (const auto &track : seq.tracks) {
    Json jsteps = Json::array();
    for (const auto &step : track.steps) {
      jsteps.push_back({
          {"active", step.active},
          {"tie", step.tie},
          {"degree", step.degree},
          {"velocity", step.velocity},
          {"midi_note", step.midi_note},
      });
    }
    jtracks.push_back({
        {"midi_channel", track.midi_channel},
        {"midi_program", track.midi_program},
        {"midi_bank", track.midi_bank},
        {"sample_bank", track.sample_bank},
        {"loop_length", track.loop_length},
        {"volume", track.volume},
        {"pan", track.pan},
        {"velocity_scale", track.velocity_scale},
        {"muted", track.muted},
        {"solo", track.solo},
        {"steps", jsteps},
    });
  }

  const Json j{
      {"bpm", seq.bpm},
      {"gate_pct", seq.gate_pct},
      {"step_count", seq.step_count},
      {"step_division", seq.step_division},
      {"tracks", jtracks},
  };

  std::filesystem::create_directories(
      std::filesystem::path{path}.parent_path());
  std::ofstream f{path};
  if (f) {
    f << j.dump(2);
    std::cout << "[session] saved\n";
  } else {
    std::cerr << "[session] failed to write: " << path << '\n';
  }
}

std::optional<Sequencer> load_session(const std::string &path) {
  std::ifstream f{path};
  if (!f) {
    return std::nullopt;
  }

  try {
    const auto j = Json::parse(f);
    Sequencer seq{};
    seq.bpm = j.value("bpm", 120.0F);
    seq.gate_pct = j.value("gate_pct", 50);
    seq.step_count = sequencer_step_count(j.value("step_count", 32));
    seq.step_division = sequencer_step_division(j.value("step_division", 16));

    if (j.contains("tracks")) {
      seq.tracks.clear();
      for (const auto &jt : j.at("tracks")) {
        SeqTrack track{};
        track.midi_channel = jt.value("midi_channel", 0);
        track.midi_program = jt.value("midi_program", 0);
        track.midi_bank = jt.value("midi_bank", 0);
        track.sample_bank = jt.value("sample_bank", -1);
        track.loop_length = jt.value("loop_length", seq.step_count);
        track.volume = jt.value("volume", 100);
        track.pan = jt.value("pan", 64);
        track.velocity_scale = jt.value("velocity_scale", 100);
        track.muted = jt.value("muted", false);
        track.solo = jt.value("solo", false);
        track.steps.resize(static_cast<std::size_t>(seq.step_count));
        if (jt.contains("steps")) {
          const auto &jsteps = jt.at("steps");
          const auto n = std::min(track.steps.size(), jsteps.size());
          for (std::size_t i = 0; i < n; ++i) {
            const auto &js = jsteps[i];
            track.steps[i] = {
                js.value("active", false),
                js.value("tie", false),
                js.value("degree", 0),
                js.value("velocity", 100),
                js.value("midi_note", -1),
            };
          }
        }
        seq.tracks.push_back(std::move(track));
      }
    }

    if (seq.tracks.empty()) {
      seq.tracks.resize(4);
      for (int i = 0; i < 4; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        seq.tracks[idx].midi_channel = i;
        seq.tracks[idx].steps.resize(static_cast<std::size_t>(seq.step_count));
        seq.tracks[idx].loop_length = seq.step_count;
      }
    }

    std::cout << "[session] loaded (" << seq.tracks.size() << " tracks)\n";
    return seq;
  } catch (const std::exception &e) {
    std::cerr << "[session] load failed: " << e.what() << '\n';
    return std::nullopt;
  }
}

} // namespace analogno
