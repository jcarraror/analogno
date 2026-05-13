#pragma once

#include "web_state.hpp"

#include <cstdint>
#include <memory>

namespace analogno {

class WebSocketServer final {
public:
  explicit WebSocketServer(std::uint16_t port = 8765);
  ~WebSocketServer();

  WebSocketServer(const WebSocketServer&) = delete;
  auto operator=(const WebSocketServer&) -> WebSocketServer& = delete;

  WebSocketServer(WebSocketServer&&) = delete;
  auto operator=(WebSocketServer&&) -> WebSocketServer& = delete;

  auto start() -> void;
  auto stop() -> void;
  auto publish(const WebRuntimeState& state) -> void;

  [[nodiscard]] auto consume_panic_requested() -> bool;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace analogno
