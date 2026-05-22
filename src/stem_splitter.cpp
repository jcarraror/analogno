#include "stem_splitter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string_view>
#include <string>
#include <utility>

namespace analogno {

namespace fs = std::filesystem;

static constexpr const char *k_out_dir = "/tmp/analogno-demucs";
static constexpr const char *k_model = "htdemucs_6s";

static std::string shell_quote(const std::string &value) {
    std::string quoted{"'"};
    for (const char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

static std::string demucs_python_command() {
    if (const char *python = std::getenv("ANALOGNO_DEMUCS_PYTHON")) {
        if (*python != '\0') {
            return shell_quote(python);
        }
    }

    for (const auto &path : {fs::path{".venv/bin/python"}, fs::path{"../.venv/bin/python"}}) {
        if (fs::exists(path)) {
            return shell_quote(path.string());
        }
    }

    return "python3";
}

static std::optional<float> parse_percent(std::string_view line) {
    const auto percent_pos = line.find('%');
    if (percent_pos == std::string_view::npos) {
        return std::nullopt;
    }

    auto begin = percent_pos;
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(line[begin - 1]))) {
        --begin;
    }
    if (begin == percent_pos) {
        return std::nullopt;
    }

    float value = 0.0F;
    for (auto i = begin; i < percent_pos; ++i) {
        value = value * 10.0F + static_cast<float>(line[i] - '0');
    }
    return std::clamp(value / 100.0F, 0.0F, 1.0F);
}

StemSplitter::~StemSplitter() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void StemSplitter::split(fs::path input) {
    if (state_.load() == StemSplitState::running) {
        return;
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    {
        const auto lock = std::scoped_lock{result_mutex_};
        result_.reset();
        error_.reset();
    }

    set_progress(0.02F, "Preparing Demucs");
    state_.store(StemSplitState::running);

    thread_ = std::thread([this, input = std::move(input)] {
        // https://github.com/facebookresearch/demucs — output: {out}/htdemucs/{track.stem}/{stem}.wav
        // nice -n 19: lowest scheduling priority so audio threads are never preempted by demucs
        // -j 1: single worker to limit parallel CPU load alongside the audio engine
        const std::string cmd =
            "nice -n 19 " + demucs_python_command() + " -m demucs"
            " -n " + k_model +
            " -j 1"
            " -o " + shell_quote(k_out_dir) +
            " " + shell_quote(input.string()) +
            " 2>&1";

        FILE *pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            const auto lock = std::scoped_lock{result_mutex_};
            error_ = "failed to spawn demucs process";
            set_progress(0.0F, "Failed to start Demucs");
            state_.store(StemSplitState::error);
            return;
        }

        set_progress(0.06F, "Loading model");
        std::array<char, 256> buf{};
        while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
            std::cout << "[demucs] " << buf.data();
            const std::string_view line{buf.data()};
            if (line.find("Separated tracks will be stored") != std::string_view::npos) {
                set_progress(std::max(progress_.load(), 0.08F), "Separating stems");
            } else if (const auto percent = parse_percent(line)) {
                set_progress(0.10F + (*percent * 0.82F), "Separating stems");
            }
        }
        std::cout << std::flush;

        if (pclose(pipe) != 0) {
            const auto lock = std::scoped_lock{result_mutex_};
            error_ = "demucs exited with an error — check the [demucs] logs and Python dependencies";
            set_progress(progress_.load(), "Demucs failed");
            state_.store(StemSplitState::error);
            return;
        }

        set_progress(0.94F, "Checking stem files");
        const auto stem_dir = fs::path{k_out_dir} / k_model / input.stem();

        StemPaths paths{
            stem_dir / "drums.wav",
            stem_dir / "bass.wav",
            stem_dir / "vocals.wav",
            stem_dir / "guitar.wav",
            stem_dir / "piano.wav",
            stem_dir / "other.wav",
        };

        for (const auto *p : {&paths.drums, &paths.bass, &paths.vocals, &paths.guitar, &paths.piano, &paths.other}) {
            if (!fs::exists(*p)) {
                const auto lock = std::scoped_lock{result_mutex_};
                error_ = "expected stem file missing: " + p->string();
                set_progress(progress_.load(), "Stem file missing");
                state_.store(StemSplitState::error);
                return;
            }
        }

        {
            const auto lock = std::scoped_lock{result_mutex_};
            result_ = std::move(paths);
        }
        set_progress(1.0F, "Stems ready");
        state_.store(StemSplitState::done);
    });
}

void StemSplitter::reset() {
    if (thread_.joinable()) {
        thread_.join();
    }
    const auto lock = std::scoped_lock{result_mutex_};
    result_.reset();
    error_.reset();
    set_progress(0.0F, "");
    state_.store(StemSplitState::idle);
}

StemSplitState StemSplitter::state() const {
    return state_.load();
}

float StemSplitter::progress() const {
    return progress_.load();
}

std::string StemSplitter::detail() const {
    const auto lock = std::scoped_lock{progress_mutex_};
    return detail_;
}

void StemSplitter::set_progress(float progress, std::string detail) {
    progress_.store(std::clamp(progress, 0.0F, 1.0F));
    const auto lock = std::scoped_lock{progress_mutex_};
    detail_ = std::move(detail);
}

std::optional<StemPaths> StemSplitter::take_result() {
    const auto lock = std::scoped_lock{result_mutex_};
    return std::exchange(result_, std::nullopt);
}

std::optional<std::string> StemSplitter::take_error() {
    const auto lock = std::scoped_lock{result_mutex_};
    return std::exchange(error_, std::nullopt);
}

} // namespace analogno
