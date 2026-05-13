#pragma once

#include <SDL3/SDL_error.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace analogno {

[[noreturn]] inline void fail_sdl(std::string_view message) {
  std::cerr << "fatal: " << message << ": " << SDL_GetError() << '\n';
  std::exit(EXIT_FAILURE);
}

inline void warn_sdl(std::string_view message) {
  std::cerr << "warning: " << message << ": " << SDL_GetError() << '\n';
}

} // namespace analogno
