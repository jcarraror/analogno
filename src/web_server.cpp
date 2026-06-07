#include "web_server.hpp"
#include "ws_codec.hpp"
#include "stem_pipeline.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

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

[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>>
parse_byte_range(std::string_view header, std::size_t total) {
  constexpr auto prefix = std::string_view{"bytes="};
  if (!header.starts_with(prefix)) return std::nullopt;
  const auto spec = header.substr(prefix.size());
  const auto dash = spec.find('-');
  if (dash == std::string_view::npos) return std::nullopt;
  const auto start_sv = spec.substr(0, dash);
  const auto end_sv   = spec.substr(dash + 1);

  std::size_t start = 0;
  std::size_t end   = total > 0 ? total - 1 : 0;

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
  std::atomic<std::uint32_t> client_generation{0};
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
    } else if (req.method() == beast::http::verb::post &&
               req.target().starts_with("/api/load-bank")) {
      handle_load_bank_upload(std::move(req));
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
    shared_->client_generation.fetch_add(1, std::memory_order_relaxed);

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

  void handle_load_bank_upload(beast::http::request<beast::http::string_body> req) {
    const auto target = std::string_view{req.target()};
    const auto q = target.find('?');
    std::size_t bank = 0;
    bool bank_valid = false;
    if (q != std::string_view::npos) {
      auto query = target.substr(q + 1);
      constexpr auto key = std::string_view{"bank="};
      if (query.starts_with(key)) {
        query = query.substr(key.size());
        for (const char c : query) {
          if (c < '0' || c > '9') break;
          bank = bank * 10 + static_cast<std::size_t>(c - '0');
          bank_valid = true;
        }
      }
    }
    if (!bank_valid || bank >= 8) {
      send_http_error(beast::http::status::bad_request, req.version());
      return;
    }

    const auto ct  = std::string_view{req[beast::http::field::content_type]};
    const auto ext = extension_for_content_type(ct);

    std::string tmp_path = std::string{"/tmp/analogno-bank-XXXXXX"} + std::string{ext};
    const int fd = mkstemps(tmp_path.data(), static_cast<int>(ext.size()));
    if (fd == -1) {
      std::cerr << "[bank-upload] failed to create temp file\n";
      send_http_error(beast::http::status::internal_server_error, req.version());
      return;
    }

    const auto& body  = req.body();
    const bool  wrote = write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size());
    close(fd);

    if (wrote) {
      enqueue_command(WebSocketServer::LoadFileToBank{.bank = bank, .path = tmp_path});
    } else {
      std::cerr << "[bank-upload] failed to write " << tmp_path << '\n';
    }

    const unsigned version = req.version();
    beast::http::response<beast::http::string_body> res{
        wrote ? beast::http::status::accepted
              : beast::http::status::internal_server_error,
        version};
    res.set(beast::http::field::server, "Analogno");
    res.set(beast::http::field::content_type, "application/json");
    res.set(beast::http::field::access_control_allow_origin, "*");
    res.body() = wrote ? R"({"ok":true})" : R"({"ok":false})";
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

    const auto is_valid = std::any_of(
        StemPipeline::stem_labels.begin(), StemPipeline::stem_labels.end(),
        [&](const char* label) { return name == std::string{label} + ".wav"; });
    if (is_valid) {
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

    if (auto cmd = ws_codec::decode(message))
      enqueue_command(std::move(*cmd));

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

  void broadcast(const std::string& message) {
    shared_->sessions.erase(
        std::remove_if(shared_->sessions.begin(), shared_->sessions.end(),
                       [](const auto& s) { return s.expired(); }),
        shared_->sessions.end());
    for (const auto& weak : shared_->sessions)
      if (const auto s = weak.lock()) s->send(message);
  }

  void publish_tick(WebTickState state) {
    net::post(shared_->ioc, [shared = shared_, state = std::move(state)]() mutable {
      shared->sessions.erase(
          std::remove_if(shared->sessions.begin(), shared->sessions.end(),
                         [](const auto& s) { return s.expired(); }),
          shared->sessions.end());
      if (shared->sessions.empty()) return;
      try {
        const auto message = ws_codec::encode(state);
        for (const auto& weak : shared->sessions)
          if (const auto s = weak.lock()) s->send(message);
      } catch (const std::exception& e) {
        std::cerr << "[ws] tick serialization error: " << e.what() << '\n';
      }
    });
  }

  void publish_runtime(WebRuntimeState state) {
    net::post(shared_->ioc, [shared = shared_, state = std::move(state)]() mutable {
      shared->sessions.erase(
          std::remove_if(shared->sessions.begin(), shared->sessions.end(),
                         [](const auto& s) { return s.expired(); }),
          shared->sessions.end());
      if (shared->sessions.empty()) return;
      try {
        const auto message = ws_codec::encode(state);
        for (const auto& weak : shared->sessions)
          if (const auto s = weak.lock()) s->send(message);
      } catch (const std::exception& e) {
        std::cerr << "[ws] publish serialization error: " << e.what() << '\n';
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
      message = ws_codec::encode(state);
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

  [[nodiscard]] std::uint32_t client_generation() const {
    return shared_->client_generation.load(std::memory_order_relaxed);
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

void WebSocketServer::publish_tick(WebTickState state) {
  impl_->publish_tick(std::move(state));
}

void WebSocketServer::publish_runtime(WebRuntimeState state) {
  impl_->publish_runtime(std::move(state));
}

std::uint32_t WebSocketServer::client_generation() const {
  return impl_->client_generation();
}

void WebSocketServer::publish_library(const WebLibraryState &state) {
  impl_->publish_library(state);
}

std::vector<WebSocketServer::Command> WebSocketServer::consume_commands() {
  return impl_->consume_commands();
}

} // namespace analogno
