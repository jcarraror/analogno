#pragma once

#include "sequencer.hpp"

#include <optional>
#include <string>

namespace analogno {

std::string default_session_path();
void save_session(const Sequencer &seq, const std::string &path);
std::optional<Sequencer> load_session(const std::string &path);

} // namespace analogno
