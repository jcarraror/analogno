#include "web_server.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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
    });
  }

  return result;
}

std::string to_json_string(const WebRuntimeState &state) {
  const Json json{
      {"type", "state"},
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
              {"banks", sample_banks_json(state.audio.banks)},
              {"activeBank", state.audio.active_bank},
              {"touchpadDrawing", state.audio.touchpad_drawing},
              {"touchpadSketch", state.audio.touchpad_sketch},
              {"touchpadRawPoints", state.audio.touchpad_raw_points},
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
                    {"degree", s.degree},
                    {"velocity", s.velocity},
                    {"midiNote", s.midi_note},
                });
              }
              tracks.push_back({
                  {"midiChannel", track.midi_channel},
                  {"midiProgram", track.midi_program},
                  {"midiBank", track.midi_bank},
                  {"muted", track.muted},
                  {"steps", std::move(steps)},
              });
            }
            return Json{
                {"playing", state.seq.playing},
                {"activeTrack", state.seq.active_track},
                {"selectedStep", state.seq.selected_step},
                {"bpm", state.seq.bpm},
                {"currentStep", state.seq.current_step},
                {"gatePct", state.seq.gate_pct},
                {"tracks", std::move(tracks)},
            };
          }(),
      },
      {"presets", [&] {
          Json arr = Json::array();
          for (const auto &p : state.presets) {
              arr.push_back({{"bank", p.bank}, {"program", p.program}, {"name", p.name}});
          }
          return arr;
      }()},
      {"soundfonts", [&] {
          Json arr = Json::array();
          for (const auto &s : state.soundfonts) arr.push_back(s);
          return arr;
      }()},
      {"activeSoundfont", state.active_soundfont},
  };

  return json.dump();
}

class SharedState final {
public:
  explicit SharedState(std::uint16_t port)
      : acceptor{ioc, tcp::endpoint{net::ip::make_address("127.0.0.1"), port}} {
  }

  net::io_context ioc{};
  tcp::acceptor acceptor;
  std::vector<std::weak_ptr<class Session>> sessions{};
  std::atomic_bool panic_requested{false};
  std::atomic_int capture_device_request{-2};
  std::atomic_int active_bank_request{-1};
  std::mutex sample_trim_mutex{};
  std::optional<WebSocketServer::SampleTrimRequest> sample_trim_request{};
  std::mutex save_sample_mutex{};
  std::optional<std::size_t> save_sample_request{};
  std::mutex patch_mutex{};
  std::optional<WebSocketServer::PatchRequest> patch_request{};
  std::mutex wavetable_mutex{};
  std::optional<std::vector<float>> wavetable_request{};
  std::atomic_bool seq_play_cmd{false};
  std::atomic_bool seq_stop_cmd{false};
  std::atomic_int  seq_select_step_cmd{-2};  // -2 = no pending; -1 = deselect; 0..15 = arm
  std::atomic_int  seq_select_track_cmd{-2}; // -2 = no pending; 0..15 = select track
  std::atomic_bool seq_add_track_cmd{false};
  std::atomic_int  seq_remove_track_cmd{-1}; // -1 = no pending; >=0 = remove that index
  std::mutex seq_config_mutex{};
  std::optional<WebSocketServer::SeqConfig> seq_config{};
  std::mutex soundfont_mutex{};
  std::optional<std::string> soundfont_request{};
};

class Session final : public std::enable_shared_from_this<Session> {
public:
  Session(tcp::socket socket, std::shared_ptr<SharedState> shared)
      : ws_{std::move(socket)}, shared_{std::move(shared)} {}

  void run() {
    ws_.set_option(
        websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws_.set_option(websocket::stream_base::decorator(
        [](websocket::response_type &response) {
          response.set(beast::http::field::server, "Analogno");
        }));

    ws_.async_accept(
        beast::bind_front_handler(&Session::on_accept, shared_from_this()));
  }

  void send(std::string message) {
    net::post(ws_.get_executor(), beast::bind_front_handler(
                                      &Session::queue_send, shared_from_this(),
                                      std::move(message)));
  }

private:
  websocket::stream<tcp::socket> ws_;
  beast::flat_buffer buffer_{};
  std::shared_ptr<SharedState> shared_;
  std::vector<std::string> write_queue_{};

  void on_accept(beast::error_code error) {
    if (error) {
      std::cerr << "websocket accept failed: " << error.message() << '\n';
      return;
    }

    do_read();
  }

  void do_read() {
    ws_.async_read(buffer_, beast::bind_front_handler(&Session::on_read,
                                                      shared_from_this()));
  }

  void on_read(beast::error_code error, std::size_t) {
    if (error == websocket::error::closed) {
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
        shared_->panic_requested.store(true);
      } else if (json.value("type", "") == "setCaptureDevice") {
        if (json.contains("deviceIndex") && json["deviceIndex"].is_number_integer()) {
          shared_->capture_device_request.store(json["deviceIndex"].get<int>());
        } else {
          shared_->capture_device_request.store(-1);
        }
      } else if (json.value("type", "") == "setSampleTrim") {
        const auto lock = std::scoped_lock{shared_->sample_trim_mutex};
        shared_->sample_trim_request = WebSocketServer::SampleTrimRequest{
            .start = json.value("start", 0.0F),
            .end = json.value("end", 1.0F),
        };
      } else if (json.value("type", "") == "setActiveBank") {
        if (json.contains("bank") && json["bank"].is_number_integer()) {
          const auto bank = json["bank"].get<int>();
          if (bank >= 0) {
            shared_->active_bank_request.store(bank);
          }
        }
      } else if (json.value("type", "") == "saveSample") {
        if (json.contains("bank") && json["bank"].is_number_integer()) {
          const auto bank = json["bank"].get<int>();
          if (bank >= 0) {
            const auto lock = std::scoped_lock{shared_->save_sample_mutex};
            shared_->save_sample_request = static_cast<std::size_t>(bank);
          }
        }
      } else if (json.value("type", "") == "setPatch") {
        const auto bank = json.value("bank", 0);
        const auto program = json.value("program", 0);
        if (bank >= 0 && bank <= 128 && program >= 0 && program < 128) {
          const auto lock = std::scoped_lock{shared_->patch_mutex};
          shared_->patch_request = WebSocketServer::PatchRequest{
              .bank = bank,
              .program = static_cast<std::uint8_t>(program),
          };
        }
      } else if (json.value("type", "") == "setSoundfont") {
        if (json.contains("path") && json["path"].is_string()) {
          shared_->soundfont_request = json["path"].get<std::string>();
        }
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
          if (!samples.empty()) {
            const auto lock = std::scoped_lock{shared_->wavetable_mutex};
            shared_->wavetable_request = std::move(samples);
          }
        }
      } else if (json.value("type", "") == "seqPlay") {
        shared_->seq_play_cmd.store(true);
      } else if (json.value("type", "") == "seqStop") {
        shared_->seq_stop_cmd.store(true);
      } else if (json.value("type", "") == "selectSeqStep") {
        const auto step = json.value("step", -1);
        shared_->seq_select_step_cmd.store(std::clamp(step, -1, 15));
      } else if (json.value("type", "") == "selectSeqTrack") {
        const auto track = json.value("track", 0);
        shared_->seq_select_track_cmd.store(std::clamp(track, 0, 15));
      } else if (json.value("type", "") == "seqAddTrack") {
        shared_->seq_add_track_cmd.store(true);
      } else if (json.value("type", "") == "seqRemoveTrack") {
        const auto track = json.value("track", -1);
        if (track >= 0) shared_->seq_remove_track_cmd.store(track);
      } else if (json.value("type", "") == "setSeq") {
        WebSocketServer::SeqConfig cfg{};
        if (json.contains("bpm") && json["bpm"].is_number()) {
          cfg.bpm = std::clamp(json["bpm"].get<float>(), 20.0F, 300.0F);
        }
        if (json.contains("gatePct") && json["gatePct"].is_number_integer()) {
          cfg.gate_pct = std::clamp(json["gatePct"].get<int>(), 5, 100);
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
            cfg.tracks[t].muted        = tj.value("muted", false);
            if (tj.contains("steps") && tj["steps"].is_array()) {
              const auto &arr = tj["steps"];
              const auto n = std::min(arr.size(),
                  static_cast<std::size_t>(WebSocketServer::SeqConfig::step_count));
              for (std::size_t i = 0; i < n; ++i) {
                const auto &s = arr[i];
                cfg.tracks[t].steps[i] = WebSocketServer::SeqStepConfig{
                    .active    = s.value("active", false),
                    .degree    = std::clamp(s.value("degree", 0), 0, 27),
                    .velocity  = std::clamp(s.value("velocity", 100), 1, 127),
                    .midi_note = s.value("midiNote", -1),
                };
              }
            }
          }
        }
        const auto lock = std::scoped_lock{shared_->seq_config_mutex};
        shared_->seq_config = cfg;
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
    ws_.text(true);

    ws_.async_write(
        net::buffer(write_queue_.front()),
        beast::bind_front_handler(&Session::on_write, shared_from_this()));
  }

  void on_write(beast::error_code error, std::size_t) {
    if (error) {
      std::cerr << "websocket write failed: " << error.message() << '\n';
      return;
    }

    write_queue_.erase(write_queue_.begin());

    if (!write_queue_.empty()) {
      do_write();
    }
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
      shared_->sessions.push_back(session);
      session->run();
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

  void publish(const WebRuntimeState &state) {
    std::string message;
    try {
      message = to_json_string(state);
    } catch (const std::exception &e) {
      std::cerr << "[ws] publish serialization error: " << e.what() << '\n';
      return;
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

  [[nodiscard]] bool consume_panic_requested() {
    return shared_->panic_requested.exchange(false);
  }

  [[nodiscard]] std::optional<int> consume_capture_device_request() {
    const auto request = shared_->capture_device_request.exchange(-2);

    if (request == -2) {
      return std::nullopt;
    }

    return request;
  }

  [[nodiscard]] std::optional<WebSocketServer::SampleTrimRequest>
  consume_sample_trim_request() {
    const auto lock = std::scoped_lock{shared_->sample_trim_mutex};
    auto request = shared_->sample_trim_request;
    shared_->sample_trim_request.reset();
    return request;
  }

  [[nodiscard]] std::optional<std::size_t> consume_active_bank_request() {
    const auto val = shared_->active_bank_request.exchange(-1);
    if (val >= 0) {
      return static_cast<std::size_t>(val);
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::size_t> consume_save_sample_request() {
    const auto lock = std::scoped_lock{shared_->save_sample_mutex};
    return std::exchange(shared_->save_sample_request, std::nullopt);
  }

  [[nodiscard]] std::optional<WebSocketServer::PatchRequest> consume_patch_request() {
    const auto lock = std::scoped_lock{shared_->patch_mutex};
    return std::exchange(shared_->patch_request, std::nullopt);
  }

  [[nodiscard]] std::optional<std::vector<float>> consume_wavetable_request() {
    const auto lock = std::scoped_lock{shared_->wavetable_mutex};
    return std::exchange(shared_->wavetable_request, std::nullopt);
  }

  [[nodiscard]] bool consume_seq_play() {
    return shared_->seq_play_cmd.exchange(false);
  }

  [[nodiscard]] bool consume_seq_stop() {
    return shared_->seq_stop_cmd.exchange(false);
  }

  [[nodiscard]] bool consume_seq_add_track() {
    return shared_->seq_add_track_cmd.exchange(false);
  }

  [[nodiscard]] std::optional<int> consume_seq_remove_track() {
    const auto v = shared_->seq_remove_track_cmd.exchange(-1);
    if (v < 0) return std::nullopt;
    return v;
  }

  [[nodiscard]] std::optional<int> consume_seq_select_step() {
    const auto v = shared_->seq_select_step_cmd.exchange(-2);
    if (v == -2) return std::nullopt;
    return v;
  }

  [[nodiscard]] std::optional<int> consume_seq_select_track() {
    const auto v = shared_->seq_select_track_cmd.exchange(-2);
    if (v == -2) return std::nullopt;
    return v;
  }

  [[nodiscard]] std::optional<WebSocketServer::SeqConfig> consume_seq_config() {
    const auto lock = std::scoped_lock{shared_->seq_config_mutex};
    return std::exchange(shared_->seq_config, std::nullopt);
  }

  [[nodiscard]] std::optional<std::string> consume_soundfont_request() {
    const auto lock = std::scoped_lock{shared_->soundfont_mutex};
    return std::exchange(shared_->soundfont_request, std::nullopt);
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

void WebSocketServer::publish(const WebRuntimeState &state) {
  impl_->publish(state);
}

bool WebSocketServer::consume_panic_requested() {
  return impl_->consume_panic_requested();
}

std::optional<int> WebSocketServer::consume_capture_device_request() {
  return impl_->consume_capture_device_request();
}

std::optional<WebSocketServer::SampleTrimRequest>
WebSocketServer::consume_sample_trim_request() {
  return impl_->consume_sample_trim_request();
}

std::optional<std::size_t> WebSocketServer::consume_active_bank_request() {
  return impl_->consume_active_bank_request();
}

std::optional<std::size_t> WebSocketServer::consume_save_sample_request() {
  return impl_->consume_save_sample_request();
}

std::optional<WebSocketServer::PatchRequest> WebSocketServer::consume_patch_request() {
  return impl_->consume_patch_request();
}

std::optional<std::vector<float>> WebSocketServer::consume_wavetable_request() {
  return impl_->consume_wavetable_request();
}

bool WebSocketServer::consume_seq_play() {
  return impl_->consume_seq_play();
}

bool WebSocketServer::consume_seq_stop() {
  return impl_->consume_seq_stop();
}

bool WebSocketServer::consume_seq_add_track() {
  return impl_->consume_seq_add_track();
}

std::optional<int> WebSocketServer::consume_seq_remove_track() {
  return impl_->consume_seq_remove_track();
}

std::optional<int> WebSocketServer::consume_seq_select_step() {
  return impl_->consume_seq_select_step();
}

std::optional<int> WebSocketServer::consume_seq_select_track() {
  return impl_->consume_seq_select_track();
}

std::optional<WebSocketServer::SeqConfig> WebSocketServer::consume_seq_config() {
  return impl_->consume_seq_config();
}

std::optional<std::string> WebSocketServer::consume_soundfont_request() {
  return impl_->consume_soundfont_request();
}

} // namespace analogno
