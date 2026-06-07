#pragma once

#include "app_context.hpp"

#include <functional>
#include <string>

namespace analogno {

using LoadStemsFn = std::function<std::size_t(const std::string&)>;

[[nodiscard]] bool dispatch_web_commands(AppContext& ctx, const LoadStemsFn& load_stems_fn);

} // namespace analogno
