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

static bool is_tqdm_bar(std::string_view line) {
    return line.find('%') != std::string_view::npos &&
           line.find('|') != std::string_view::npos;
}

// Parse N/M fraction from tqdm bar lines
static std::optional<float> parse_tqdm_fraction(std::string_view line) {
    const auto slash = line.find('/');
    if (slash == std::string_view::npos) return std::nullopt;

    auto num_end = slash;
    while (num_end > 0 && (std::isdigit(static_cast<unsigned char>(line[num_end - 1])) || line[num_end - 1] == '.'))
        --num_end;

    auto den_end = slash + 1;
    while (den_end < line.size() && (std::isdigit(static_cast<unsigned char>(line[den_end])) || line[den_end] == '.'))
        ++den_end;

    if (num_end >= slash || den_end == slash + 1) return std::nullopt;

    float num = 0.0F, den = 0.0F, frac = 0.1F;
    bool in_frac = false;
    for (auto i = num_end; i < slash; ++i) {
        if (line[i] == '.') { in_frac = true; continue; }
        if (in_frac) { num += static_cast<float>(line[i] - '0') * frac; frac *= 0.1F; }
        else num = num * 10.0F + static_cast<float>(line[i] - '0');
    }
    frac = 0.1F; in_frac = false;
    for (auto i = slash + 1; i < den_end; ++i) {
        if (line[i] == '.') { in_frac = true; continue; }
        if (in_frac) { den += static_cast<float>(line[i] - '0') * frac; frac *= 0.1F; }
        else den = den * 10.0F + static_cast<float>(line[i] - '0');
    }
    if (den <= 0.0F) return std::nullopt;
    return std::clamp(num / den, 0.0F, 1.0F);
}

// Parse ETA from "[00:30<01:45, ...]" suffixes in tqdm lines
static std::string parse_eta(std::string_view line) {
    const auto lt = line.find('<');
    if (lt == std::string_view::npos) return {};
    const auto comma = line.find(',', lt);
    if (comma == std::string_view::npos) return {};
    const auto eta = line.substr(lt + 1, comma - lt - 1);
    // eta is "MM:SS"
    const auto colon = eta.find(':');
    if (colon == std::string_view::npos) return {};
    int mins = 0, secs = 0;
    for (auto i = std::size_t{0}; i < colon; ++i)
        mins = mins * 10 + (eta[i] - '0');
    for (auto i = colon + 1; i < eta.size(); ++i)
        secs = secs * 10 + (eta[i] - '0');
    std::string result;
    if (mins > 0) result = std::to_string(mins) + "m ";
    result += std::to_string(secs) + "s";
    return result;
}

static std::optional<int> parse_bag_count(std::string_view line) {
    const auto pos = line.find("bag of ");
    if (pos == std::string_view::npos) return std::nullopt;
    const auto start = pos + 7;
    int n = 0; bool found = false;
    for (auto i = start; i < line.size() && std::isdigit(static_cast<unsigned char>(line[i])); ++i) {
        n = n * 10 + (line[i] - '0'); found = true;
    }
    return found ? std::optional{n} : std::nullopt;
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
            "nice -n 19 " + demucs_python_command() +
            " -m demucs"
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

        constexpr auto k_sep_start = 0.10F;
        constexpr auto k_sep_end   = 0.92F;

        bool separation_started = false;
        int  expected_passes    = 1;
        int  pass_num           = 0;
        float pass_max_frac     = 0.0F;

        std::array<char, 512> buf{};
        while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
            std::cout << "[demucs] " << buf.data();
            std::string_view line{buf.data()};

            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.remove_suffix(1);
            if (line.empty()) continue;

            if (!is_tqdm_bar(line)) {
                append_log(std::string{line});
                if (const auto n = parse_bag_count(line))
                    expected_passes = std::max(1, *n);
            }

            if (!separation_started) {
                if (line.find("Separated tracks will be stored") != std::string_view::npos ||
                    line.find("Separating track") != std::string_view::npos) {
                    separation_started = true;
                    set_progress(k_sep_start, "Separating stems…");
                } else if (line.find("Downloading") != std::string_view::npos) {
                    set_progress(0.04F, "Downloading model…");
                } else if (line.find("Loading") != std::string_view::npos ||
                           line.find("loading") != std::string_view::npos) {
                    set_progress(0.07F, "Loading model…");
                }
                continue;
            }

            if (is_tqdm_bar(line)) {
                if (const auto frac = parse_tqdm_fraction(line)) {
                    if (*frac + 0.15F < pass_max_frac) {
                        pass_num = std::min(pass_num + 1, expected_passes - 1);
                        pass_max_frac = 0.0F;
                    }
                    pass_max_frac = std::max(pass_max_frac, *frac);
                    const auto per_pass = (k_sep_end - k_sep_start) / static_cast<float>(expected_passes);
                    const auto p = k_sep_start + (static_cast<float>(pass_num) + *frac) * per_pass;
                    const auto eta = parse_eta(line);
                    const auto detail = eta.empty() ? "Separating stems…"
                                                    : "Separating stems… " + eta + " left";
                    set_progress(std::min(p, k_sep_end), detail);
                }
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
    {
        const auto lock = std::scoped_lock{result_mutex_};
        result_.reset();
        error_.reset();
    }
    {
        const auto lock = std::scoped_lock{progress_mutex_};
        log_lines_.clear();
        detail_.clear();
    }
    progress_.store(0.0F, std::memory_order_relaxed);
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
    const auto clamped = std::clamp(progress, 0.0F, 1.0F);
    float current = progress_.load(std::memory_order_relaxed);
    while (clamped > current &&
           !progress_.compare_exchange_weak(current, clamped,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {}
    const auto lock = std::scoped_lock{progress_mutex_};
    detail_ = std::move(detail);
}

void StemSplitter::append_log(std::string line) {
    const auto lock = std::scoped_lock{progress_mutex_};
    log_lines_.push_back(std::move(line));
    while (log_lines_.size() > k_max_log_lines)
        log_lines_.pop_front();
}

std::vector<std::string> StemSplitter::log() const {
    const auto lock = std::scoped_lock{progress_mutex_};
    return {log_lines_.begin(), log_lines_.end()};
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
