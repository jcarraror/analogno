#include "audio_sampler.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <thread>
#include <vector>

using namespace analogno;
using namespace std::chrono;

static std::vector<float> make_sine(float hz, float secs, std::uint32_t rate = 48000U) {
    const auto n = static_cast<std::size_t>(secs * static_cast<float>(rate));
    std::vector<float> buf(n * 2U);
    for (std::size_t i = 0; i < n; ++i) {
        const auto t    = static_cast<float>(i) / static_cast<float>(rate);
        const auto s    = std::sin(2.0F * std::numbers::pi_v<float> * hz * t) * 0.5F;
        buf[i * 2U]     = s;
        buf[i * 2U + 1U] = s;
    }
    return buf;
}

static void print_sep() {
    std::cout << std::string(64, '-') << '\n';
}

struct Scenario {
    const char* label;
    int         tracks;      // banks to trigger simultaneously per step
    int         step_ms;     // step period in ms (simulated BPM + division)
    int         duration_s;
};

static void run_scenario(AudioSampler& s, const Scenario& sc) {
    s.stop_all();
    s.perf_reset();
    std::this_thread::sleep_for(milliseconds{60}); // let audio thread drain

    print_sep();
    std::cout << "  " << sc.label << '\n';
    print_sep();

    const auto end_time = steady_clock::now() + seconds{sc.duration_s};
    int       total_triggers = 0;
    std::uint64_t trigger_ns_sum = 0;
    std::uint64_t trigger_ns_max = 0;

    while (steady_clock::now() < end_time) {
        const auto step_t0 = steady_clock::now();

        for (int t = 0; t < sc.tracks; ++t) {
            const auto bank = static_cast<std::size_t>(
                t % static_cast<int>(AudioSampler::bank_count));
            s.release_bank(bank, 1.0F, -1);
            const auto trig_t0 = steady_clock::now();
            s.trigger_bank(bank, 1.0F, -1);
            const auto dt = static_cast<std::uint64_t>(
                duration_cast<nanoseconds>(steady_clock::now() - trig_t0).count());
            trigger_ns_sum += dt;
            trigger_ns_max  = std::max(trigger_ns_max, dt);
            ++total_triggers;
        }

        const auto step_elapsed = steady_clock::now() - step_t0;
        const auto wait = milliseconds{sc.step_ms} - duration_cast<milliseconds>(step_elapsed);
        if (wait > milliseconds{0})
            std::this_thread::sleep_for(wait);
    }

    const auto ps = s.perf_snapshot();

    // Audio callback budget = frame_count / sample_rate.
    // miniaudio defaults to 480 frames @ 48 kHz → 10 ms per callback.
    constexpr double kBudgetUs = 10'000.0;

    const double avg_load = ps.callback_count > 0U
        ? ps.avg_callback_us / kBudgetUs * 100.0 : 0.0;
    const double max_load = ps.callback_count > 0U
        ? ps.max_callback_us / kBudgetUs * 100.0 : 0.0;
    const double trig_avg_us = total_triggers > 0
        ? static_cast<double>(trigger_ns_sum) / static_cast<double>(total_triggers) / 1000.0
        : 0.0;

    std::cout << std::fixed << std::setprecision(1);
    std::cout
        << "  step period:          " << sc.step_ms << " ms  ("
        << static_cast<double>(1000) / static_cast<double>(sc.step_ms) << " steps/s)\n"
        << "  triggers fired:       " << total_triggers << '\n'
        << "  trigger avg:          " << trig_avg_us << " µs"
        << "   (mutex + voice alloc)\n"
        << "  trigger max:          "
        << static_cast<double>(trigger_ns_max) / 1000.0 << " µs\n"
        << "  audio callbacks:      " << ps.callback_count << '\n'
        << "  callback avg:         " << ps.avg_callback_us << " µs"
        << "  (" << avg_load << "% of " << static_cast<int>(kBudgetUs / 1000.0) << " ms budget)\n"
        << "  callback max:         " << ps.max_callback_us << " µs"
        << "  (" << max_load << "% of budget)\n"
        << "  peak active voices:   " << ps.peak_voices
        << " / " << AudioSampler::voice_count << '\n'
        << "  voice steals:         " << ps.voice_steals;
    if (ps.voice_steals > 0U)
        std::cout << "  *** ALL SLOTS EXHAUSTED — increase voice_count ***";
    std::cout << "\n\n";
}

int main() {
    std::cout << "\n=== analogno AudioSampler stress benchmark ===\n\n";
    std::cout << "voice_count = " << AudioSampler::voice_count << '\n';
    std::cout << "bank_count  = " << AudioSampler::bank_count  << "\n\n";

    AudioSampler sampler;
    if (!sampler.is_running()) {
        std::cerr << "FATAL: AudioSampler failed to start (no audio device?)\n";
        return 1;
    }

    // Load all 8 banks with 0.5-second stereo sine waves at different pitches.
    std::cout << "Loading " << AudioSampler::bank_count << " sample banks (0.5 s each)...\n\n";
    constexpr std::array<float, AudioSampler::bank_count> kFreqs{
        110.0F, 165.0F, 220.0F, 330.0F, 440.0F, 660.0F, 880.0F, 1320.0F
    };
    for (std::size_t b = 0; b < AudioSampler::bank_count; ++b)
        sampler.set_sample_for_bank(b, make_sine(kFreqs[b], 0.5F), 2U);
    sampler.set_gain(0.10F); // quiet — this is a test

    // --- Realistic sequencer scenarios ---
    const Scenario kScenarios[] = {
        { "4 tracks  @ 120 bpm / 1-16   (125 ms steps)  5 s", 4, 125, 5 },
        { "8 tracks  @ 120 bpm / 1-16   (125 ms steps)  5 s", 8, 125, 5 },
        { "8 tracks  @ 160 bpm / 1-16   ( 93 ms steps)  5 s", 8,  93, 5 },
        { "8 tracks  @ 120 bpm / 1-32   ( 62 ms steps)  5 s", 8,  62, 5 },
        { "8 tracks  @ 200 bpm / 1-32   ( 37 ms steps)  5 s", 8,  37, 5 },
    };
    for (const auto& sc : kScenarios)
        run_scenario(sampler, sc);

    // --- Voice exhaustion test: triggers > voice_count ---
    {
        sampler.stop_all();
        sampler.perf_reset();
        print_sep();
        const int kOver = static_cast<int>(AudioSampler::voice_count) + 8;
        std::cout << "  Voice exhaustion: trigger " << kOver
                  << " times (> voice_count=" << AudioSampler::voice_count << ")\n";
        print_sep();
        for (int i = 0; i < kOver; ++i)
            sampler.trigger_bank(
                static_cast<std::size_t>(i % static_cast<int>(AudioSampler::bank_count)),
                1.0F + static_cast<float>(i) * 0.03F,  // unique rate per trigger → bypasses retrigger path
                -1);
        std::this_thread::sleep_for(milliseconds{200});
        const auto ps = sampler.perf_snapshot();
        std::cout << "  peak active voices:   " << ps.peak_voices
                  << " / " << AudioSampler::voice_count << '\n'
                  << "  voice steals:         " << ps.voice_steals;
        if (ps.voice_steals > 0U)
            std::cout << "  (expected: " << (kOver - static_cast<int>(AudioSampler::voice_count))
                      << " steal(s))";
        std::cout << "\n\n";
    }

    print_sep();
    std::cout << "Done.\n\n";
    return 0;
}
