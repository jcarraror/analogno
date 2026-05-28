#pragma once

#include "sdl_gamepad.hpp"
#include "web_state.hpp"

#include <optional>
#include <string>
#include <vector>

namespace analogno {

struct Sdl final {
    Sdl();
    ~Sdl();
    Sdl(const Sdl&)            = delete;
    Sdl& operator=(const Sdl&) = delete;
    Sdl(Sdl&&)                 = delete;
    Sdl& operator=(Sdl&&)      = delete;
};

void signal_quit() noexcept;

[[nodiscard]] std::vector<std::string> scan_soundfonts();
[[nodiscard]] std::optional<Gamepad>   open_first_gamepad();

void run_event_loop(Gamepad&                        gamepad,
                    std::vector<WebPreset>          sf2_presets,
                    std::string                     active_soundfont,
                    const std::vector<std::string>& available_soundfonts);

} // namespace analogno
