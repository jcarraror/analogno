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

  struct PatchRequest final {
    std::uint8_t bank{};
    std::uint8_t program{};
  };

  explicit WebSocketServer(std::uint16_t port = 8765);
  ~WebSocketServer();

  WebSocketServer(const WebSocketServer &) = delete;
  WebSocketServer &operator=(const WebSocketServer &) = delete;

  WebSocketServer(WebSocketServer &&) = delete;
  WebSocketServer &operator=(WebSocketServer &&) = delete;

  void start();
  void stop();
  void publish(const WebRuntimeState &state);

  [[nodiscard]] bool consume_panic_requested();
  [[nodiscard]] std::optional<int> consume_capture_device_request();
  [[nodiscard]] std::optional<SampleTrimRequest> consume_sample_trim_request();
  [[nodiscard]] std::optional<std::size_t> consume_active_bank_request();
  [[nodiscard]] std::optional<std::size_t> consume_save_sample_request();
  [[nodiscard]] std::optional<PatchRequest> consume_patch_request();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace analogno
