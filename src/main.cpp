#include "event_loop.hpp"
#include "sf2_reader.hpp"

#include <SDL3/SDL_sensor.h>

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
void handle_signal(int) { analogno::signal_quit(); }
} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::string soundfont_path{};
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--soundfont" && i + 1 < argc)
            soundfont_path = argv[++i];
    }

    auto sf2_presets = soundfont_path.empty()
        ? std::vector<analogno::WebPreset>{}
        : analogno::read_sf2_presets(soundfont_path);

    if (!sf2_presets.empty())
        std::cout << "loaded " << sf2_presets.size()
                  << " presets from " << soundfont_path << '\n';

    const auto available_soundfonts = analogno::scan_soundfonts();
    const analogno::Sdl sdl{};
    auto gamepad = analogno::open_first_gamepad();
    if (!gamepad) return EXIT_FAILURE;

    std::cout << "gamepad: " << gamepad->name() << '\n';
    gamepad->enable_sensor(SDL_SENSOR_GYRO,  "gyro");
    gamepad->enable_sensor(SDL_SENSOR_ACCEL, "accelerometer");

    analogno::run_event_loop(*gamepad, std::move(sf2_presets),
                             std::move(soundfont_path), available_soundfonts);
    return EXIT_SUCCESS;
}
