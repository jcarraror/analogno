#pragma once

#include "sequencer.hpp"

#include <optional>
#include <string>

namespace analogno {

struct SessionData {
  Sequencer seq{};
  std::string stems_folder{};
};

std::string default_session_path();
void save_session(const Sequencer &seq, const std::string &stems_folder,
                  const std::string &path);
std::optional<SessionData> load_session(const std::string &path);

} // namespace analogno
