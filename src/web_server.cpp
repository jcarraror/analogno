#include "web_server.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace analogno {
namespace {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;

using tcp = net::ip::tcp;
using Json = nlohmann::json;

Json capture_devices_json(const std::vector<WebCaptureDevice> &devices) {
  auto result = Json::array();

  for (const auto &device : devices) {
    result.push_back({
        {"index", device.index},
        {"name", device.name},
        {"isDefault", device.is_default},
    });
  }

  return result;
}

Json sample_banks_json(const std::vector<WebSampleBank> &banks) {
  auto result = Json::array();

  for (const auto &bank : banks) {
    result.push_back({
        {"hasData", bank.has_sample},
        {"frames", bank.frames},
        {"trimStart", bank.trim_start},
        {"trimEnd", bank.trim_end},
        {"isWavetable", bank.is_wavetable},
        {"isStream", bank.is_stream},
        {"waveform", bank.waveform},
        {"rootNote", bank.root_note},
        {"sliceCount", bank.slice_count},
    });
  }

  return result;
}

std::string runtime_json_string(const WebRuntimeState &state) {
  const Json json{
      {"type", "runtime"},
      {
          "controller",
          {
              {"leftX", state.controller.left_x},
              {"leftY", state.controller.left_y},
              {"rightX", state.controller.right_x},
              {"rightY", state.controller.right_y},
              {"l2", state.controller.left_trigger},
              {"r2", state.controller.right_trigger},
              {"hasGyro", state.controller.has_gyro},
              {"hasAccel", state.controller.has_accel},
              {
                  "gyro",
                  {
                      {"x", state.controller.gyro.x},
                      {"y", state.controller.gyro.y},
                      {"z", state.controller.gyro.z},
                  },
              },
              {
                  "accel",
                  {
                      {"x", state.controller.accel.x},
                      {"y", state.controller.accel.y},
                      {"z", state.controller.accel.z},
                  },
              },
          },
      },
      {
          "music",
          {
              {"rootMidiNote", state.music.root_midi_note},
              {"octaveOffset", state.music.octave_offset},
              {"scale", state.music.scale},
              {"pitchBend", state.music.pitch_bend},
              {"expression", state.music.expression},
              {"filterCutoff", state.music.filter_cutoff},
              {"filterResonance", state.music.filter_resonance},
              {"modulation", state.music.modulation},
              {"vibrato", state.music.vibrato},
              {"activeNotes", state.music.active_notes},
              {"midiProgram", state.music.midi_program},
              {"midiBank", state.music.midi_bank},
              {"buttonMidiNotes", state.music.button_midi_notes},
          },
      },
      {
          "audio",
          {
              {"devices", capture_devices_json(state.audio.devices)},
              {"selectedDeviceIndex", state.audio.selected_device_index},
              {"captureRunning", state.audio.capture_running},
              {"sampleRecording", state.audio.sample_recording},
              {"captureDevice", state.audio.capture_device},
              {"micLevel", state.audio.mic_level},
              {"envelope", state.audio.envelope},
              {"gateOpen", state.audio.gate_open},
              {"onset", state.audio.onset},
              {"velocity", state.audio.velocity},
              {"waveform", state.audio.waveform},
              {"sampleReady", state.audio.sample_ready},
              {"sampleFrames", state.audio.sample_frames},
              {"sampleTrimStart", state.audio.sample_trim_start},
              {"sampleTrimEnd", state.audio.sample_trim_end},
              {"sampleWaveform", state.audio.sample_waveform},
              {"wavetableCycle", state.audio.wavetable_cycle},
              {"banks", sample_banks_json(state.audio.banks)},
              {"activeBank", state.audio.active_bank},
              {"touchpadDrawing", state.audio.touchpad_drawing},
              {"touchpadSketch", state.audio.touchpad_sketch},
              {"touchpadRawPoints", state.audio.touchpad_raw_points},
              {"signalsVolume", state.audio.signals_volume},
              {"blowMode", state.audio.blow_mode},
              {"wavetableMorph", state.audio.wavetable_morph},
              {"wavetableNoise", state.audio.wavetable_noise},
              {"wavetableUnison", state.audio.wavetable_unison},
              {"blowSensitivity", state.audio.blow_sensitivity},
              {"blowActive", state.audio.blow_active},
              {"blowLevel", state.audio.blow_level},
              {"voiceSeqAvailable", state.audio.voice_seq_available},
              {"voiceSeqCompiled", state.audio.voice_seq_compiled},
              {"voiceSeqEnabled", state.audio.voice_seq_enabled},
              {"voiceSeqRecording", state.audio.voice_seq_recording},
              {"voiceSeqMode", state.audio.voice_seq_mode},
              {"voiceSeqSnap", state.audio.voice_seq_snap},
              {"voiceSeqSensitivity", state.audio.voice_seq_sensitivity},
              {"voiceSeqTimingOffsetMs",
               state.audio.voice_seq_timing_offset_ms},
              {"voiceSeqLastNote", state.audio.voice_seq_last_note},
              {"voiceSeqLastVelocity", state.audio.voice_seq_last_velocity},
              {"voiceSeqAcceptedNotes", state.audio.voice_seq_accepted_notes},
              {"voiceSeqRejectedNotes", state.audio.voice_seq_rejected_notes},
              {"voiceSeqRecordedSegments",
               state.audio.voice_seq_recorded_segments},
              {"voiceSeqRecordProgress", state.audio.voice_seq_record_progress},
              {"specSamples", state.audio.spec_samples},
              {"stemSplitState", state.audio.stem_split_state},
              {"stemSplitError", state.audio.stem_split_error},
              {"stemSplitProgress", state.audio.stem_split_progress},
              {"stemSplitDetail", state.audio.stem_split_detail},
              {"stemSplitLog", state.audio.stem_split_log},
              {"stemsFolder", state.audio.stems_folder},
              {"stems", [&] {
                auto arr = Json::array();
                for (const auto &s : state.audio.stems) {
                  arr.push_back({
                      {"name", s.name},
                      {"frames", s.frames},
                      {"isActive", s.is_active},
                      {"waveform", s.waveform},
                  });
                }
                return arr;
              }()},
          },
      },
      {
          "seq",
          [&] {
            auto tracks = Json::array();
            for (const auto &track : state.seq.tracks) {
              auto steps = Json::array();
              for (const auto &s : track.steps) {
                steps.push_back({
                    {"active", s.active},
                    {"tie", s.tie},
                    {"degree", s.degree},
                    {"velocity", s.velocity},
                    {"midiNote", s.midi_note},
                });
              }
              tracks.push_back({
                  {"midiChannel", track.midi_channel},
                  {"midiProgram", track.midi_program},
                  {"midiBank", track.midi_bank},
                  {"sampleBank", track.sample_bank},
                  {"loopLength", track.loop_length},
                  {"volume", track.volume},
                  {"pan", track.pan},
                  {"velocityScale", track.velocity_scale},
                  {"muted", track.muted},
                  {"solo", track.solo},
                  {"steps", std::move(steps)},
              });
            }
            return Json{
                {"playing", state.seq.playing},
                {"activeTrack", state.seq.active_track},
                {"selectedStep", state.seq.selected_step},
                {"bpm", state.seq.bpm},
                {"playheadStep", state.seq.playhead_step},
                {"currentStep", state.seq.current_step},
                {"gatePct", state.seq.gate_pct},
                {"stepCount", state.seq.step_count},
                {"stepDivision", state.seq.step_division},
                {"tracks", std::move(tracks)},
            };
          }(),
      },
      {"pianoRollVisible", state.piano_roll_visible},
      {"spectrogramVisible", state.spectrogram_visible},
  };

  return json.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string library_json_string(const WebLibraryState &state) {
  const Json json{
      {"type", "library"},
      {"presets",
       [&] {
         Json arr = Json::array();
         for (const auto &p : state.presets) {
           arr.push_back(
               {{"bank", p.bank}, {"program", p.program}, {"name", p.name}});
         }
         return arr;
       }()},
      {"soundfonts",
       [&] {
         Json arr = Json::array();
         for (const auto &s : state.soundfonts) {
           arr.push_back(s);
         }
         return arr;
       }()},
      {"activeSoundfont", state.active_soundfont},
  };

  return json.dump(-1, ' ', false, Json::error_handler_t::replace);
}

class SharedState final {
public:
  explicit SharedState(std::uint16_t port)
      : acceptor{ioc, tcp::endpoint{net::ip::make_address("127.0.0.1"), port}} {
  }

  net::io_context ioc{};
  tcp::acceptor acceptor;
  std::vector<std::weak_ptr<class Session>> sessions{};
  std::mutex command_mutex{};
  std::vector<WebSocketServer::Command> commands{};
  std::mutex library_message_mutex{};
  std::optional<std::string> last_library_message{};
  std::mutex stems_folder_mutex{};
  std::string stems_folder{};
};

class Session final : public std::enable_shared_from_this<Session> {
public:
  Session(tcp::socket socket, std::shared_ptr<SharedState> shared)
      : stream_{std::move(socket)}, shared_{std::move(shared)} {}

  void run() {
    // https://www.boost.org/doc/libs/release/libs/beast/example/advanced/server/
    // Read the HTTP request first — then branch on WebSocket upgrade vs plain HTTP.
    parser_.emplace();
    parser_->body_limit(256ULL * 1024 * 1024); // 256 MB for audio uploads
    beast::http::async_read(
        stream_, buffer_, *parser_,
        beast::bind_front_handler(&Session::on_http_read, shared_from_this()));
  }

  void send(std::string message) {
    net::post(ws_->get_executor(), beast::bind_front_handler(
                                       &Session::queue_send, shared_from_this(),
                                       std::move(message)));
  }

private:
  beast::tcp_stream stream_;
  beast::flat_buffer buffer_{};
  std::shared_ptr<SharedState> shared_;

  std::optional<beast::http::request_parser<beast::http::string_body>> parser_{};
  std::optional<websocket::stream<beast::tcp_stream>> ws_{};
  std::vector<std::string> write_queue_{};

  void enqueue_command(WebSocketServer::Command command) {
    const auto lock = std::scoped_lock{shared_->command_mutex};
    shared_->commands.push_back(std::move(command));
  }

  void on_http_read(beast::error_code ec, std::size_t) {
    if (ec) {
      return;
    }
    auto req = parser_->release();
    parser_.reset();

    if (websocket::is_upgrade(req)) {
      upgrade_to_ws(std::move(req));
    } else if (req.method() == beast::http::verb::options) {
      send_cors_preflight();
    } else if (req.method() == beast::http::verb::post &&
               req.target() == "/api/split-audio") {
      handle_audio_upload(std::move(req));
    } else if (req.method() == beast::http::verb::get &&
               req.target().starts_with("/api/stems/")) {
      handle_stem_request(std::move(req));
    } else {
      send_http_error(beast::http::status::not_found, req.version());
    }
  }

  void upgrade_to_ws(beast::http::request<beast::http::string_body> req) {
    ws_.emplace(std::move(stream_));
    ws_->set_option(
        websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws_->set_option(websocket::stream_base::decorator(
        [](websocket::response_type &response) {
          response.set(beast::http::field::server, "Analogno");
        }));
    ws_->async_accept(
        req,
        beast::bind_front_handler(&Session::on_accept, shared_from_this()));
  }

  void on_accept(beast::error_code error) {
    if (error) {
      std::cerr << "websocket accept failed: " << error.message() << '\n';
      return;
    }

    shared_->sessions.push_back(shared_from_this());

    {
      const auto lock = std::scoped_lock{shared_->library_message_mutex};
      if (shared_->last_library_message.has_value()) {
        queue_send(*shared_->last_library_message);
      }
    }

    do_read();
  }

  void do_read() {
    ws_->async_read(buffer_, beast::bind_front_handler(&Session::on_read,
                                                       shared_from_this()));
  }

  static std::string_view extension_for_content_type(std::string_view ct) {
    if (ct.find("flac") != std::string_view::npos) return ".flac";
    if (ct.find("ogg") != std::string_view::npos) return ".ogg";
    if (ct.find("mp3") != std::string_view::npos ||
        ct.find("mpeg") != std::string_view::npos) return ".mp3";
    return ".wav";
  }

  void handle_audio_upload(beast::http::request<beast::http::string_body> req) {
    namespace fs = std::filesystem;

    const auto ct = std::string_view{req[beast::http::field::content_type]};
    const auto ext = extension_for_content_type(ct);

    std::string tmp_path = std::string{"/tmp/analogno-upload-XXXXXX"} + std::string{ext};
    const int fd = mkstemps(tmp_path.data(), static_cast<int>(ext.size()));
    if (fd == -1) {
      std::cerr << "[upload] failed to create temp file\n";
      return;
    }

    const auto &body = req.body();
    const bool wrote =
        write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size());
    close(fd);

    const bool ok = wrote;
    if (ok) {
      enqueue_command(WebSocketServer::SplitAudioFile{.path = tmp_path});
    } else {
      std::cerr << "[upload] failed to write " << tmp_path << '\n';
    }

    const unsigned version = req.version();
    beast::http::response<beast::http::string_body> res{
        ok ? beast::http::status::accepted
           : beast::http::status::internal_server_error,
        version};
    res.set(beast::http::field::server, "Analogno");
    res.set(beast::http::field::content_type, "application/json");
    res.set(beast::http::field::access_control_allow_origin, "*");
    res.body() = ok ? R"({"ok":true})" : R"({"ok":false})";
    res.prepare_payload();

    auto res_ptr = std::make_shared<beast::http::response<beast::http::string_body>>(
        std::move(res));
    beast::http::async_write(
        stream_, *res_ptr,
        [self = shared_from_this(), res_ptr](beast::error_code, std::size_t) {});
  }

  void send_cors_preflight() {
    beast::http::response<beast::http::empty_body> res{
        beast::http::status::no_content, 11};
    res.set(beast::http::field::server, "Analogno");
    res.set(beast::http::field::access_control_allow_origin, "*");
    res.set(beast::http::field::access_control_allow_methods, "GET, POST, OPTIONS");
    res.set(beast::http::field::access_control_allow_headers, "Content-Type, Range");
    res.set(beast::http::field::access_control_max_age, "86400");
    res.prepare_payload();

    auto res_ptr = std::make_shared<beast::http::response<beast::http::empty_body>>(
        std::move(res));
    beast::http::async_write(
        stream_, *res_ptr,
        [self = shared_from_this(), res_ptr](beast::error_code, std::size_t) {});
  }

  static std::optional<std::pair<std::size_t, std::size_t>>
  parse_byte_range(std::string_view header, std::size_t total) {
    constexpr auto prefix = std::string_view{"bytes="};
    if (!header.starts_with(prefix)) return std::nullopt;
    const auto spec = header.substr(prefix.size());
    const auto dash = spec.find('-');
    if (dash == std::string_view::npos) return std::nullopt;
    const auto start_sv = spec.substr(0, dash);
    const auto end_sv = spec.substr(dash + 1);

    std::size_t start = 0;
    std::size_t end = total > 0 ? total - 1 : 0;

    if (start_sv.empty()) {
      if (end_sv.empty() || total == 0) return std::nullopt;
      std::size_t suffix = 0;
      for (char c : end_sv) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
        suffix = suffix * 10 + static_cast<std::size_t>(c - '0');
      }
      start = suffix >= total ? 0 : total - suffix;
    } else {
      for (char c : start_sv) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
        start = start * 10 + static_cast<std::size_t>(c - '0');
      }
      if (!end_sv.empty()) {
        end = 0;
        for (char c : end_sv) {
          if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
          end = end * 10 + static_cast<std::size_t>(c - '0');
        }
      }
    }

    if (total == 0 || start >= total || start > end) return std::nullopt;
    end = std::min(end, total - 1);
    return std::make_pair(start, end);
  }

  std::optional<std::filesystem::path> stem_path_for_target(
      std::string_view target) {
    const auto prefix = std::string_view{"/api/stems/"};
    if (!target.starts_with(prefix)) {
      return std::nullopt;
    }

    auto name = target.substr(prefix.size());
    if (const auto query = name.find('?'); query != std::string_view::npos) {
      name = name.substr(0, query);
    }

    if (name == "drums.wav" || name == "bass.wav" || name == "vocals.wav" ||
        name == "guitar.wav" || name == "piano.wav" || name == "other.wav") {
      const auto lock = std::scoped_lock{shared_->stems_folder_mutex};
      const auto &folder = shared_->stems_folder;
      if (folder.empty()) return std::nullopt;
      return std::filesystem::path{folder} / std::string{name};
    }

    return std::nullopt;
  }

  void handle_stem_request(beast::http::request<beast::http::string_body> req) {
    const auto target = req.target();
    const auto maybe_path =
        stem_path_for_target(std::string_view{target.data(), target.size()});
    const unsigned version = req.version();
    if (!maybe_path.has_value()) {
      send_http_error(beast::http::status::not_found, version);
      return;
    }

    // Open the file first to get its size
    beast::error_code open_ec;
    beast::http::file_body::value_type file;
    file.open(maybe_path->string().c_str(), beast::file_mode::scan, open_ec);
    if (open_ec) {
      send_http_error(beast::http::status::not_found, version);
      return;
    }
    const auto total = file.size();

    const auto range_str = std::string{req[beast::http::field::range]};
    const auto maybe_range = range_str.empty()
        ? std::optional<std::pair<std::size_t, std::size_t>>{}
        : parse_byte_range(range_str, total);

    // Unsatisfiable range
    if (!range_str.empty() && !maybe_range) {
      file.close();
      beast::http::response<beast::http::string_body> res{
          beast::http::status::range_not_satisfiable, version};
      res.set(beast::http::field::server, "Analogno");
      res.set(beast::http::field::access_control_allow_origin, "*");
      res.set(beast::http::field::accept_ranges, "bytes");
      res.set(beast::http::field::content_range, "bytes */" + std::to_string(total));
      res.set(beast::http::field::connection, "close");
      res.prepare_payload();
      auto res_ptr = std::make_shared<beast::http::response<beast::http::string_body>>(std::move(res));
      beast::http::async_write(stream_, *res_ptr,
          [self = shared_from_this(), res_ptr](beast::error_code, std::size_t) {});
      return;
    }

    const bool is_full = !maybe_range ||
        (maybe_range->first == 0 && maybe_range->second == total - 1);

    if (!is_full) {
      file.close();
      const auto [start, end] = *maybe_range;
      const auto len = end - start + 1;

      std::ifstream in{*maybe_path, std::ios::binary};
      in.seekg(static_cast<std::streamoff>(start));
      std::string data(len, '\0');
      in.read(data.data(), static_cast<std::streamsize>(len));

      beast::http::response<beast::http::string_body> res{
          beast::http::status::partial_content, version};
      res.set(beast::http::field::server, "Analogno");
      res.set(beast::http::field::content_type, "audio/wav");
      res.set(beast::http::field::access_control_allow_origin, "*");
      res.set(beast::http::field::accept_ranges, "bytes");
      res.set(beast::http::field::content_range,
          "bytes " + std::to_string(start) + "-" +
          std::to_string(end) + "/" + std::to_string(total));
      res.set(beast::http::field::cache_control, "no-store");
      res.set(beast::http::field::connection, "close");
      res.body() = std::move(data);
      res.prepare_payload();
      auto res_ptr = std::make_shared<beast::http::response<beast::http::string_body>>(std::move(res));
      beast::http::async_write(stream_, *res_ptr,
          [self = shared_from_this(), res_ptr](beast::error_code ec, std::size_t) {
            if (!ec) {
              beast::error_code shutdown_ec;
              self->stream_.socket().shutdown(tcp::socket::shutdown_send, shutdown_ec);
            }
          });
      return;
    }

    const bool is_partial = maybe_range.has_value();
    beast::http::response<beast::http::file_body> res{
        is_partial ? beast::http::status::partial_content : beast::http::status::ok,
        version};
    res.set(beast::http::field::server, "Analogno");
    res.set(beast::http::field::content_type, "audio/wav");
    res.set(beast::http::field::access_control_allow_origin, "*");
    res.set(beast::http::field::accept_ranges, "bytes");
    res.set(beast::http::field::cache_control, "no-store");
    res.set(beast::http::field::connection, "close");
    if (is_partial) {
      res.set(beast::http::field::content_range,
          "bytes 0-" + std::to_string(total - 1) + "/" + std::to_string(total));
    }
    res.body() = std::move(file);
    res.prepare_payload();

    auto res_ptr =
        std::make_shared<beast::http::response<beast::http::file_body>>(std::move(res));
    beast::http::async_write(
        stream_, *res_ptr,
        [self = shared_from_this(), res_ptr](beast::error_code ec, std::size_t) {
          if (!ec) {
            beast::error_code shutdown_ec;
            self->stream_.socket().shutdown(tcp::socket::shutdown_send, shutdown_ec);
          }
        });
  }

  void send_http_error(beast::http::status status, unsigned version) {
    beast::http::response<beast::http::string_body> res{status, version};
    res.set(beast::http::field::server, "Analogno");
    res.set(beast::http::field::content_type, "text/plain");
    res.set(beast::http::field::access_control_allow_origin, "*");
    const auto reason = beast::http::obsolete_reason(status);
    res.body() = std::string{reason.data(), reason.size()};
    res.prepare_payload();

    auto res_ptr = std::make_shared<beast::http::response<beast::http::string_body>>(
        std::move(res));
    beast::http::async_write(
        stream_, *res_ptr,
        [self = shared_from_this(), res_ptr](beast::error_code, std::size_t) {});
  }

  void on_read(beast::error_code error, std::size_t) {
    if (is_expected_disconnect(error)) {
      return;
    }

    if (error) {
      std::cerr << "websocket read failed: " << error.message() << '\n';
      return;
    }

    const auto message = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());

    try {
      const auto json = Json::parse(message);

      if (json.value("type", "") == "panic") {
        enqueue_command(WebSocketServer::Panic{});
      } else if (json.value("type", "") == "setCaptureDevice") {
        if (json.contains("deviceIndex") && json["deviceIndex"].is_number_integer()) {
          enqueue_command(WebSocketServer::SetCaptureDevice{
              .device_index = json["deviceIndex"].get<int>()});
        } else {
          enqueue_command(WebSocketServer::SetCaptureDevice{
              .device_index = std::nullopt});
        }
      } else if (json.value("type", "") == "setSampleTrim") {
        enqueue_command(WebSocketServer::SetSampleTrim{
            .trim = WebSocketServer::SampleTrimRequest{
                .start = json.value("start", 0.0F),
                .end = json.value("end", 1.0F),
            }});
      } else if (json.value("type", "") == "setActiveBank") {
        if (json.contains("bank") && json["bank"].is_number_integer()) {
          const auto bank = json["bank"].get<int>();
          if (bank >= 0) {
            enqueue_command(WebSocketServer::SetActiveBank{
                .bank = static_cast<std::size_t>(bank)});
          }
        }
      } else if (json.value("type", "") == "saveSample") {
        if (json.contains("bank") && json["bank"].is_number_integer()) {
          const auto bank = json["bank"].get<int>();
          if (bank >= 0) {
            enqueue_command(WebSocketServer::SaveSample{
                .bank = static_cast<std::size_t>(bank)});
          }
        }
      } else if (json.value("type", "") == "stemPlay") {
        const auto idx = json.value("idx", -1);
        if (idx >= 0) {
          enqueue_command(WebSocketServer::StemPlay{.idx = static_cast<std::size_t>(idx)});
        }
      } else if (json.value("type", "") == "stemStop") {
        const auto idx = json.value("idx", -1);
        if (idx >= 0) {
          enqueue_command(WebSocketServer::StemStop{.idx = static_cast<std::size_t>(idx)});
        }
      } else if (json.value("type", "") == "setStemFolder") {
        const auto path = json.value("path", std::string{});
        if (!path.empty()) {
          enqueue_command(WebSocketServer::SetStemFolder{.path = path});
        }
      } else if (json.value("type", "") == "setActiveStem") {
        const auto idx = json.value("idx", -1);
        if (idx >= 0) {
          enqueue_command(WebSocketServer::SetActiveStem{.idx = static_cast<std::size_t>(idx)});
        }
      } else if (json.value("type", "") == "setPatch") {
        const auto bank = json.value("bank", 0);
        const auto program = json.value("program", 0);
        if (bank >= 0 && bank <= 128 && program >= 0 && program < 128) {
          enqueue_command(WebSocketServer::SetPatch{
              .patch = WebSocketServer::PatchRequest{
                  .bank = bank,
                  .program = static_cast<std::uint8_t>(program),
              }});
        }
      } else if (json.value("type", "") == "setSoundfont") {
        if (json.contains("path") && json["path"].is_string()) {
          enqueue_command(WebSocketServer::SetSoundfont{
              .path = json["path"].get<std::string>()});
        }
      } else if (json.value("type", "") == "setBlowMode") {
        enqueue_command(WebSocketServer::SetBlowMode{
            .enabled = json.value("enabled", false)});
      } else if (json.value("type", "") == "setBlowSensitivity") {
        const auto v = std::clamp(json.value("sensitivity", 50), 0, 100);
        enqueue_command(WebSocketServer::SetBlowSensitivity{
            .sensitivity = static_cast<float>(v) / 100.0F});
      } else if (json.value("type", "") == "setVoiceSeq") {
        enqueue_command(WebSocketServer::SetVoiceSeq{
            .config = WebSocketServer::VoiceSeqConfig{
                .enabled = json.value("enabled", false),
                .recording = json.value("recording", false),
                .mode =
                    [&] {
                      auto mode =
                          json.value("mode", std::string{"percussion"});
                      if (mode != "percussion" && mode != "harmonic" &&
                          mode != "hybrid") {
                        mode = "percussion";
                      }
                      return mode;
                    }(),
                .snap_to_scale = json.value("snapToScale", true),
                .sensitivity = std::clamp(json.value("sensitivity", 65.0F),
                                          0.0F, 100.0F) /
                               100.0F,
                .timing_offset_ms = std::clamp(
                    json.value("timingOffsetMs", 0.0F), -120.0F, 120.0F),
            }});
      } else if (json.value("type", "") == "setWavetable") {
        if (json.contains("data") && json["data"].is_array()) {
          std::vector<float> samples;
          samples.reserve(json["data"].size());
          for (const auto &v : json["data"]) {
            if (v.is_number()) {
              samples.push_back(
                  std::clamp(v.get<float>(), -1.0F, 1.0F));
            }
          }
          std::vector<float> morph_samples;
          if (json.contains("morphData") && json["morphData"].is_array()) {
            morph_samples.reserve(json["morphData"].size());
            for (const auto &v : json["morphData"]) {
              if (v.is_number()) {
                morph_samples.push_back(
                    std::clamp(v.get<float>(), -1.0F, 1.0F));
              }
            }
          }
          if (!samples.empty()) {
            enqueue_command(WebSocketServer::SetWavetable{
                .wavetable = WebSocketServer::WavetableRequest{
                    .samples = std::move(samples),
                    .morph_samples = std::move(morph_samples),
                }});
          }
        }
      } else if (json.value("type", "") == "setWavetableControls") {
        enqueue_command(WebSocketServer::SetWavetableControls{
            .controls =
                WebSocketServer::WavetableControls{
                    .morph =
                        std::clamp(json.value("morph", 0.0F), 0.0F, 1.0F),
                    .noise =
                        std::clamp(json.value("noise", 0.0F), 0.0F, 1.0F),
                    .unison =
                        std::clamp(json.value("unison", 0.0F), 0.0F, 1.0F),
                }});
      } else if (json.value("type", "") == "seqPlay") {
        enqueue_command(WebSocketServer::SeqPlay{});
      } else if (json.value("type", "") == "seqStop") {
        enqueue_command(WebSocketServer::SeqStop{});
      } else if (json.value("type", "") == "selectSeqStep") {
        const auto step = json.value("step", -1);
        enqueue_command(
            WebSocketServer::SeqSelectStep{.step = std::clamp(step, -1, 63)});
      } else if (json.value("type", "") == "selectSeqTrack") {
        const auto track = json.value("track", 0);
        enqueue_command(WebSocketServer::SeqSelectTrack{
            .track = std::clamp(track, 0, 15)});
      } else if (json.value("type", "") == "seqAddTrack") {
        enqueue_command(WebSocketServer::SeqAddTrack{});
      } else if (json.value("type", "") == "seqRemoveTrack") {
        const auto track = json.value("track", -1);
        if (track >= 0) {
          enqueue_command(WebSocketServer::SeqRemoveTrack{.track = track});
        }
      } else if (json.value("type", "") == "setSeq") {
        WebSocketServer::SeqConfig cfg{};
        if (json.contains("bpm") && json["bpm"].is_number()) {
          cfg.bpm = std::clamp(json["bpm"].get<float>(), 20.0F, 300.0F);
        }
        if (json.contains("gatePct") && json["gatePct"].is_number_integer()) {
          cfg.gate_pct = std::clamp(json["gatePct"].get<int>(), 5, 100);
        }
        if (json.contains("stepCount") && json["stepCount"].is_number_integer()) {
          const auto value = json["stepCount"].get<int>();
          if (value <= 8) cfg.step_count = 8;
          else if (value <= 16) cfg.step_count = 16;
          else if (value <= 32) cfg.step_count = 32;
          else cfg.step_count = WebSocketServer::SeqConfig::max_step_count;
        }
        if (json.contains("stepDivision") &&
            json["stepDivision"].is_number_integer()) {
          const auto value = json["stepDivision"].get<int>();
          if (value <= 8) cfg.step_division = 8;
          else if (value <= 16) cfg.step_division = 16;
          else cfg.step_division = 32;
        }
        if (json.contains("tracks") && json["tracks"].is_array()) {
          const auto &tarr = json["tracks"];
          const auto nt = std::min(tarr.size(),
              static_cast<std::size_t>(WebSocketServer::SeqConfig::max_tracks));
          cfg.tracks.resize(nt);
          for (std::size_t t = 0; t < nt; ++t) {
            const auto &tj = tarr[t];
            cfg.tracks[t].midi_channel = std::clamp(tj.value("midiChannel", -1), -1, 15);
            cfg.tracks[t].midi_program = std::clamp(tj.value("midiProgram", 0), 0, 127);
            cfg.tracks[t].midi_bank    = std::clamp(tj.value("midiBank",    0), 0, 127);
            cfg.tracks[t].sample_bank  = std::clamp(tj.value("sampleBank", -1), -1, 7);
            cfg.tracks[t].loop_length  = std::clamp(tj.value("loopLength", cfg.step_count), 1, cfg.step_count);
            cfg.tracks[t].volume         = std::clamp(tj.value("volume", 100), 0, 127);
            cfg.tracks[t].pan            = std::clamp(tj.value("pan", 64), 0, 127);
            cfg.tracks[t].velocity_scale = std::clamp(tj.value("velocityScale", 100), 50, 200);
            cfg.tracks[t].muted          = tj.value("muted", false);
            cfg.tracks[t].solo           = tj.value("solo", false);
            if (tj.contains("steps") && tj["steps"].is_array()) {
              const auto &arr = tj["steps"];
              const auto n = std::min(arr.size(),
                  static_cast<std::size_t>(cfg.step_count));
              cfg.tracks[t].steps.resize(static_cast<std::size_t>(cfg.step_count));
              for (std::size_t i = 0; i < n; ++i) {
                const auto &s = arr[i];
                cfg.tracks[t].steps[i] = WebSocketServer::SeqStepConfig{
                    .active    = s.value("active", false),
                    .tie       = s.value("tie", false),
                    .degree    = std::clamp(s.value("degree", 0), 0, 27),
                    .velocity  = std::clamp(s.value("velocity", 100), 1, 127),
                    .midi_note = s.value("midiNote", -1),
                };
              }
            }
          }
        }
        enqueue_command(WebSocketServer::SetSeq{.config = std::move(cfg)});
      } else if (json.value("type", "") == "setTrackVolume") {
        const auto track = json.value("track", -1);
        const auto volume = std::clamp(json.value("volume", 100), 0, 127);
        if (track >= 0 && track < 16) {
          enqueue_command(WebSocketServer::SetTrackVolume{.track = track, .volume = volume});
        }
      } else if (json.value("type", "") == "setTrackPan") {
        const auto track = json.value("track", -1);
        const auto pan = std::clamp(json.value("pan", 64), 0, 127);
        if (track >= 0 && track < 16) {
          enqueue_command(WebSocketServer::SetTrackPan{.track = track, .pan = pan});
        }
      } else if (json.value("type", "") == "setTrackVelocityScale") {
        const auto track = json.value("track", -1);
        const auto scale = std::clamp(json.value("scale", 100), 50, 200);
        if (track >= 0 && track < 16) {
          enqueue_command(WebSocketServer::SetTrackVelocityScale{.track = track, .scale = scale});
        }
      } else if (json.value("type", "") == "setTrackSolo") {
        const auto track = json.value("track", -1);
        if (track >= 0 && track < 16) {
          enqueue_command(WebSocketServer::SetTrackSolo{
              .track = track, .solo = json.value("solo", false)});
        }
      } else if (json.value("type", "") == "setSignalsVolume") {
        const auto v = std::clamp(json.value("volume", 100), 0, 100);
        enqueue_command(WebSocketServer::SetSignalsVolume{
            .volume = static_cast<float>(v) / 100.0F});
      } else if (json.value("type", "") == "splitAudioFile") {
        if (json.contains("path") && json["path"].is_string()) {
          enqueue_command(WebSocketServer::SplitAudioFile{
              .path = json["path"].get<std::string>()});
        }
      } else if (json.value("type", "") == "streamPlayBank") {
        const auto bank = json.value("bank", -1);
        if (bank >= 0) {
          enqueue_command(WebSocketServer::StreamPlayBank{
              .bank = static_cast<std::size_t>(bank)});
        }
      } else if (json.value("type", "") == "streamStopBank") {
        const auto bank = json.value("bank", -1);
        if (bank >= 0) {
          enqueue_command(WebSocketServer::StreamStopBank{
              .bank = static_cast<std::size_t>(bank)});
        }
      } else if (json.value("type", "") == "downloadAudio") {
        if (json.contains("source") && json["source"].is_string()) {
          enqueue_command(WebSocketServer::DownloadAudio{
              .source = json["source"].get<std::string>()});
        }
      } else if (json.value("type", "") == "openStemFolderDialog") {
        enqueue_command(WebSocketServer::OpenStemFolderDialog{});
      } else if (json.value("type", "") == "loadStemToBank") {
        const auto stem_idx = json.value("stemIdx", -1);
        const auto bank = json.value("bank", -1);
        if (stem_idx >= 0 && bank >= 0) {
          enqueue_command(WebSocketServer::LoadStemToBank{
              .stem_idx = static_cast<std::size_t>(stem_idx),
              .bank = static_cast<std::size_t>(bank),
              .trim_start = std::clamp(json.value("trimStart", 0.0F), 0.0F, 1.0F),
              .trim_end = std::clamp(json.value("trimEnd", 1.0F), 0.0F, 1.0F),
          });
        }
      } else if (json.value("type", "") == "setBankRootNote") {
        const auto bank = json.value("bank", -1);
        const auto note = std::clamp(json.value("note", 48), 0, 127);
        if (bank >= 0 && bank < 8) {
          enqueue_command(WebSocketServer::SetBankRootNote{.bank = bank, .note = note});
        }
      } else if (json.value("type", "") == "setBankSliceCount") {
        const auto bank = json.value("bank", -1);
        const auto count = std::clamp(json.value("count", 0), 0, 64);
        if (bank >= 0 && bank < 8) {
          enqueue_command(WebSocketServer::SetBankSliceCount{.bank = bank, .count = count});
        }
      } else if (json.value("type", "") == "transcribeBankToSeq") {
        const auto bank = json.value("bank", -1);
        if (bank >= 0 && bank < 8) {
          enqueue_command(WebSocketServer::TranscribeBankToSeq{.bank = bank});
        }
      }
    } catch (const std::exception &exception) {
      std::cerr << "invalid websocket JSON: " << exception.what() << '\n';
    }

    do_read();
  }

  void queue_send(std::string message) {
    const auto already_writing = !write_queue_.empty();
    write_queue_.push_back(std::move(message));

    if (!already_writing) {
      do_write();
    }
  }

  void do_write() {
    ws_->text(true);

    ws_->async_write(
        net::buffer(write_queue_.front()),
        beast::bind_front_handler(&Session::on_write, shared_from_this()));
  }

  void on_write(beast::error_code error, std::size_t) {
    if (is_expected_disconnect(error)) {
      return;
    }

    if (error) {
      std::cerr << "websocket write failed: " << error.message() << '\n';
      return;
    }

    write_queue_.erase(write_queue_.begin());

    if (!write_queue_.empty()) {
      do_write();
    }
  }

  static bool is_expected_disconnect(const beast::error_code &error) {
    return error == websocket::error::closed ||
           error == net::error::eof ||
           error == net::error::connection_reset ||
           error == net::error::operation_aborted ||
           error == beast::error::timeout;
  }
};

class Listener final : public std::enable_shared_from_this<Listener> {
public:
  explicit Listener(std::shared_ptr<SharedState> shared)
      : shared_{std::move(shared)} {}

  void run() { do_accept(); }

private:
  std::shared_ptr<SharedState> shared_;

  void do_accept() {
    shared_->acceptor.async_accept(
        net::make_strand(shared_->ioc),
        beast::bind_front_handler(&Listener::on_accept, shared_from_this()));
  }

  void on_accept(beast::error_code error, tcp::socket socket) {
    if (!error) {
      auto session = std::make_shared<Session>(std::move(socket), shared_);
      session->run();
      // Sessions add themselves to shared_->sessions after WebSocket upgrade.
      // HTTP-only sessions (file uploads) are never added.
    } else {
      std::cerr << "websocket acceptor failed: " << error.message() << '\n';
    }

    shared_->sessions.erase(
        std::remove_if(shared_->sessions.begin(), shared_->sessions.end(),
                       [](const auto &session) { return session.expired(); }),
        shared_->sessions.end());

    do_accept();
  }
};

} // namespace

class WebSocketServer::Impl final {
public:
  explicit Impl(std::uint16_t port)
      : shared_{std::make_shared<SharedState>(port)} {}

  ~Impl() { stop(); }

  void start() {
    if (running_) {
      return;
    }

    running_ = true;

    std::make_shared<Listener>(shared_)->run();

    thread_ = std::thread{[shared = shared_] { shared->ioc.run(); }};

    std::cout << "websocket server listening: ws://127.0.0.1:"
              << shared_->acceptor.local_endpoint().port() << '\n';
  }

  void stop() {
    if (!running_) {
      return;
    }

    running_ = false;
    shared_->ioc.stop();

    if (thread_.joinable()) {
      thread_.join();
    }
  }

  void publish_runtime(WebRuntimeState state) {
    net::post(shared_->ioc, [shared = shared_, state = std::move(state)]() mutable {
      shared->sessions.erase(
          std::remove_if(shared->sessions.begin(), shared->sessions.end(),
                         [](const auto &s) { return s.expired(); }),
          shared->sessions.end());

      if (shared->sessions.empty()) {
        return;
      }

      std::string message;
      try {
        message = runtime_json_string(state);
      } catch (const std::exception &e) {
        std::cerr << "[ws] publish serialization error: " << e.what() << '\n';
        return;
      }

      for (const auto &weak_session : shared->sessions) {
        if (const auto session = weak_session.lock()) {
          session->send(message);
        }
      }
    });
  }

  void set_stems_folder(const std::string &path) {
    const auto lock = std::scoped_lock{shared_->stems_folder_mutex};
    shared_->stems_folder = path;
  }

  void publish_library(const WebLibraryState &state) {
    std::string message;
    try {
      message = library_json_string(state);
    } catch (const std::exception &e) {
      std::cerr << "[ws] publish serialization error: " << e.what() << '\n';
      return;
    }

    {
      const auto lock = std::scoped_lock{shared_->library_message_mutex};
      shared_->last_library_message = message;
    }

    net::post(shared_->ioc, [shared = shared_, message] {
      shared->sessions.erase(
          std::remove_if(shared->sessions.begin(), shared->sessions.end(),
                         [](const auto &session) { return session.expired(); }),
          shared->sessions.end());

      for (const auto &weak_session : shared->sessions) {
        if (const auto session = weak_session.lock()) {
          session->send(message);
        }
      }
    });
  }

  [[nodiscard]] std::vector<WebSocketServer::Command> consume_commands() {
    const auto lock = std::scoped_lock{shared_->command_mutex};
    return std::exchange(shared_->commands, {});
  }

private:
  std::shared_ptr<SharedState> shared_;
  std::thread thread_{};
  bool running_{};
};

WebSocketServer::WebSocketServer(std::uint16_t port)
    : impl_{std::make_unique<Impl>(port)} {}

WebSocketServer::~WebSocketServer() = default;

void WebSocketServer::start() { impl_->start(); }

void WebSocketServer::stop() { impl_->stop(); }

void WebSocketServer::set_stems_folder(const std::string &path) {
  impl_->set_stems_folder(path);
}

void WebSocketServer::publish_runtime(WebRuntimeState state) {
  impl_->publish_runtime(std::move(state));
}

void WebSocketServer::publish_library(const WebLibraryState &state) {
  impl_->publish_library(state);
}

std::vector<WebSocketServer::Command> WebSocketServer::consume_commands() {
  return impl_->consume_commands();
}

} // namespace analogno
