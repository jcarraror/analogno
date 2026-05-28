#pragma once

#include "music_types.hpp"

namespace analogno {
struct AppContext;

void route_live_notes(AppContext& ctx, const AudioFeatures& audio_features);
} // namespace analogno
