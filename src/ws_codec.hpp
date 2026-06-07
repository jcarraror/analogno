#pragma once

#include "web_server.hpp"
#include "web_state.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace analogno::ws_codec {

[[nodiscard]] std::string encode(const WebTickState& state);
[[nodiscard]] std::string encode(const WebRuntimeState& state);
[[nodiscard]] std::string encode(const WebLibraryState& state);

[[nodiscard]] std::optional<WebSocketServer::Command> decode(std::string_view message);

} // namespace analogno::ws_codec
