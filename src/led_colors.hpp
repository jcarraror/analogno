#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace analogno {

[[nodiscard]] inline std::array<std::uint8_t, 3> program_led_color(int program) {
    const auto family = std::clamp(program, 0, 127) / 8;
    const float hue   = static_cast<float>(family) / 16.0F;
    const float h6    = hue * 6.0F;
    const float frac  = h6 - std::floor(h6);
    const auto sector = static_cast<int>(h6) % 6;
    float r{}, g{}, b{};
    switch (sector) {
    case 0: r = 1;       g = frac;   b = 0;      break;
    case 1: r = 1 - frac; g = 1;     b = 0;      break;
    case 2: r = 0;       g = 1;      b = frac;   break;
    case 3: r = 0;       g = 1-frac; b = 1;      break;
    case 4: r = frac;    g = 0;      b = 1;      break;
    default: r = 1;      g = 0;      b = 1-frac; break;
    }
    return {
        static_cast<std::uint8_t>(r * 255.0F),
        static_cast<std::uint8_t>(g * 255.0F),
        static_cast<std::uint8_t>(b * 255.0F),
    };
}

[[nodiscard]] inline std::array<std::uint8_t, 3> sequencer_led_color(int program) {
    const auto base = program_led_color(program);
    const auto peak = std::max({base[0], base[1], base[2]});
    if (peak == 0) return {255, 255, 255};
    auto saturate = [peak](std::uint8_t v) -> std::uint8_t {
        const float n = static_cast<float>(v) / static_cast<float>(peak);
        return static_cast<std::uint8_t>(
            std::clamp(static_cast<int>(std::lround(std::pow(n, 1.75F) * 255.0F)), 0, 255));
    };
    return {saturate(base[0]), saturate(base[1]), saturate(base[2])};
}

} // namespace analogno
