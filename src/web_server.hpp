#pragma once

#include "web_state.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace analogno {

class WebSocketServer final {
public:
  struct SampleTrimRequest final {
    float start{};
    float end{1.0F};
  };

  explicit WebSocketServer(std::uint16_t port = 8765);
  ~WebSocketServer();

  WebSocketServer(const WebSocketServer &) = delete;
  auto operator=(const WebSocketServer &) -> WebSocketServer & = delete;

  WebSocketServer(WebSocketServer &&) = delete;
  auto operator=(WebSocketServer &&) -> WebSocketServer & = delete;

  auto start() -> void;
  auto stop() -> void;
  auto publish(const WebRuntimeState &state) -> void;

  [[nodiscard]] auto consume_panic_requested() -> bool;
  [[nodiscard]] auto consume_capture_device_request() -> std::optional<int>;
  [[nodiscard]] auto consume_sample_trim_request()
      -> std::optional<SampleTrimRequest>;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace analogno
