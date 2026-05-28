#pragma once

#include <SDL3/SDL_events.h>

namespace analogno {
struct AppContext;
void handle_touchpad_event(const SDL_Event& event, AppContext& ctx);
} // namespace analogno
