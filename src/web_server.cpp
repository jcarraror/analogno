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

auto to_json_string(const WebRuntimeState& state) -> std::string
{
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
      },
    },
  };

  return json.dump();
}

class SharedState final {
public:
  explicit SharedState(std::uint16_t port)
    : acceptor{ioc, tcp::endpoint{net::ip::make_address("127.0.0.1"), port}}
  {
  }

  net::io_context ioc{};
  tcp::acceptor acceptor;
  std::vector<std::weak_ptr<class Session>> sessions{};
  std::atomic_bool panic_requested{false};
};

class Session final : public std::enable_shared_from_this<Session> {
public:
  Session(tcp::socket socket, std::shared_ptr<SharedState> shared)
    : ws_{std::move(socket)}
    , shared_{std::move(shared)}
  {
  }

  auto run() -> void
  {
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws_.set_option(websocket::stream_base::decorator([](websocket::response_type& response) {
      response.set(beast::http::field::server, "Analogno");
    }));

    ws_.async_accept(
      beast::bind_front_handler(&Session::on_accept, shared_from_this())
    );
  }

  auto send(std::string message) -> void
  {
    net::post(
      ws_.get_executor(),
      beast::bind_front_handler(&Session::queue_send, shared_from_this(), std::move(message))
    );
  }

private:
  websocket::stream<tcp::socket> ws_;
  beast::flat_buffer buffer_{};
  std::shared_ptr<SharedState> shared_;
  std::vector<std::string> write_queue_{};

  auto on_accept(beast::error_code error) -> void
  {
    if (error) {
      std::cerr << "websocket accept failed: " << error.message() << '\n';
      return;
    }

    do_read();
  }

  auto do_read() -> void
  {
    ws_.async_read(
      buffer_,
      beast::bind_front_handler(&Session::on_read, shared_from_this())
    );
  }

  auto on_read(beast::error_code error, std::size_t) -> void
  {
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
      }
    } catch (const std::exception& exception) {
      std::cerr << "invalid websocket JSON: " << exception.what() << '\n';
    }

    do_read();
  }

  auto queue_send(std::string message) -> void
  {
    const auto already_writing = !write_queue_.empty();
    write_queue_.push_back(std::move(message));

    if (!already_writing) {
      do_write();
    }
  }

  auto do_write() -> void
  {
    ws_.text(true);

    ws_.async_write(
      net::buffer(write_queue_.front()),
      beast::bind_front_handler(&Session::on_write, shared_from_this())
    );
  }

  auto on_write(beast::error_code error, std::size_t) -> void
  {
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
    : shared_{std::move(shared)}
  {
  }

  auto run() -> void
  {
    do_accept();
  }

private:
  std::shared_ptr<SharedState> shared_;

  auto do_accept() -> void
  {
    shared_->acceptor.async_accept(
      net::make_strand(shared_->ioc),
      beast::bind_front_handler(&Listener::on_accept, shared_from_this())
    );
  }

  auto on_accept(beast::error_code error, tcp::socket socket) -> void
  {
    if (!error) {
      auto session = std::make_shared<Session>(std::move(socket), shared_);
      shared_->sessions.push_back(session);
      session->run();
    } else {
      std::cerr << "websocket acceptor failed: " << error.message() << '\n';
    }

    shared_->sessions.erase(
      std::remove_if(
        shared_->sessions.begin(),
        shared_->sessions.end(),
        [](const auto& session) {
          return session.expired();
        }
      ),
      shared_->sessions.end()
    );

    do_accept();
  }
};

} // namespace

class WebSocketServer::Impl final {
public:
  explicit Impl(std::uint16_t port)
    : shared_{std::make_shared<SharedState>(port)}
  {
  }

  ~Impl()
  {
    stop();
  }

  auto start() -> void
  {
    if (running_) {
      return;
    }

    running_ = true;

    std::make_shared<Listener>(shared_)->run();

    thread_ = std::thread{[shared = shared_] {
      shared->ioc.run();
    }};

    std::cout << "websocket server listening: ws://127.0.0.1:"
              << shared_->acceptor.local_endpoint().port() << '\n';
  }

  auto stop() -> void
  {
    if (!running_) {
      return;
    }

    running_ = false;
    shared_->ioc.stop();

    if (thread_.joinable()) {
      thread_.join();
    }
  }

  auto publish(const WebRuntimeState& state) -> void
  {
    const auto message = to_json_string(state);

    net::post(shared_->ioc, [shared = shared_, message] {
      shared->sessions.erase(
        std::remove_if(
          shared->sessions.begin(),
          shared->sessions.end(),
          [](const auto& session) {
            return session.expired();
          }
        ),
        shared->sessions.end()
      );

      for (const auto& weak_session : shared->sessions) {
        if (const auto session = weak_session.lock()) {
          session->send(message);
        }
      }
    });
  }

  [[nodiscard]] auto consume_panic_requested() -> bool
  {
    return shared_->panic_requested.exchange(false);
  }

private:
  std::shared_ptr<SharedState> shared_;
  std::thread thread_{};
  bool running_{};
};

WebSocketServer::WebSocketServer(std::uint16_t port)
  : impl_{std::make_unique<Impl>(port)}
{
}

WebSocketServer::~WebSocketServer() = default;

auto WebSocketServer::start() -> void
{
  impl_->start();
}

auto WebSocketServer::stop() -> void
{
  impl_->stop();
}

auto WebSocketServer::publish(const WebRuntimeState& state) -> void
{
  impl_->publish(state);
}

auto WebSocketServer::consume_panic_requested() -> bool
{
  return impl_->consume_panic_requested();
}

} // namespace analogno
