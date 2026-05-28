#include "stem_pipeline.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>

namespace analogno {

void StemPipeline::start(const std::string& source) {
    status_.error.clear();
    status_.log.clear();
    status_.progress = 0.0F;

    if (AudioDownloader::is_url(source)) {
        downloader_.download(source);
        status_.state = "downloading";
        status_.detail = "Starting download";
        std::cout << "[dl] downloading: " << source << '\n';
    } else {
        splitter_.split(source);
        status_.state = "running";
        status_.detail = "Queued";
        std::cout << "[stem] splitting: " << source << '\n';
    }
}

void StemPipeline::mark_stems_ready() {
    status_.state = "done";
    status_.progress = 1.0F;
    status_.detail = "Stems ready";
}

std::optional<std::string> StemPipeline::tick(const std::string& base_dir) {
    if (status_.state == "downloading") {
        const auto dl = downloader_.state();
        if (dl == DownloadState::done) {
            auto path = downloader_.take_result();
            downloader_.reset();
            if (path) {
                splitter_.split(*path);
                status_.state = "running";
                status_.progress = 0.0F;
                status_.detail = "Queued";
                status_.log.clear();
                std::cout << "[dl] handing off to demucs: " << path->string() << '\n';
            }
        } else if (dl == DownloadState::error) {
            auto err = downloader_.take_error();
            status_.error = err.value_or("unknown error");
            status_.state = "error";
            status_.progress = downloader_.progress();
            status_.detail = downloader_.detail();
            status_.log = downloader_.log();
            std::cerr << "[dl] error: " << status_.error << '\n';
            downloader_.reset();
        } else if (dl == DownloadState::running) {
            status_.progress = downloader_.progress();
            status_.detail = downloader_.detail();
            status_.log = downloader_.log();
        }
    }

    if (status_.state == "running") {
        const auto split = splitter_.state();
        if (split == StemSplitState::done) {
            return handle_split_done(base_dir);
        } else if (split == StemSplitState::error) {
            auto err = splitter_.take_error();
            status_.error = err.value_or("unknown error");
            status_.state = "error";
            status_.progress = splitter_.progress();
            status_.detail = splitter_.detail();
            status_.log = splitter_.log();
            std::cerr << "[stem] error: " << status_.error << '\n';
            splitter_.reset();
        } else if (split == StemSplitState::running) {
            status_.progress = splitter_.progress();
            status_.detail = splitter_.detail();
            status_.log = splitter_.log();
        }
    }

    return std::nullopt;
}

std::optional<std::string> StemPipeline::handle_split_done(const std::string& base_dir) {
    auto paths = splitter_.take_result();
    if (!paths) {
        status_.state = "error";
        status_.error = "splitter returned no paths";
        return std::nullopt;
    }

    const std::array<const std::filesystem::path*, 6> src{
        &paths->drums, &paths->bass, &paths->vocals,
        &paths->guitar, &paths->piano, &paths->other};

    const auto track_name = paths->drums.parent_path().filename().string();
    auto dest = std::filesystem::path{base_dir} / track_name;
    if (std::filesystem::exists(dest)) {
        int n = 2;
        while (std::filesystem::exists(
            std::filesystem::path{base_dir} / (track_name + "_" + std::to_string(n))))
            ++n;
        dest = std::filesystem::path{base_dir} / (track_name + "_" + std::to_string(n));
    }

    prune_old_folders(std::filesystem::path{base_dir});

    std::error_code ec;
    std::filesystem::create_directories(dest, ec);
    if (!ec) {
        for (std::size_t i = 0; i < src.size(); ++i) {
            const auto dst = dest / (std::string{stem_labels[i]} + ".wav");
            std::error_code copy_ec;
            std::filesystem::copy_file(*src[i], dst,
                std::filesystem::copy_options::overwrite_existing, copy_ec);
            if (copy_ec)
                std::cerr << "[stem] copy failed (" << stem_labels[i]
                          << "): " << copy_ec.message() << '\n';
        }
    }

    status_.state = "done";
    status_.progress = 1.0F;
    status_.detail = "Stems ready";
    status_.log = splitter_.log();
    splitter_.reset();

    return dest.string();
}

void StemPipeline::prune_old_folders(const std::filesystem::path& base) {
    using entry_t = std::pair<std::filesystem::file_time_type, std::filesystem::path>;
    std::vector<entry_t> dirs;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator{base, ec}) {
        if (e.is_directory(ec))
            dirs.emplace_back(e.last_write_time(ec), e.path());
    }
    if (dirs.size() > max_stem_folders) {
        std::sort(dirs.begin(), dirs.end());
        for (std::size_t i = 0; i + max_stem_folders < dirs.size(); ++i) {
            std::error_code rm_ec;
            std::filesystem::remove_all(dirs[i].second, rm_ec);
            std::cout << "[stems] pruned: " << dirs[i].second.filename() << '\n';
        }
    }
}

std::size_t load_stems(const std::string& folder, AudioSampler& sampler) {
    sampler.clear_stems();
    std::size_t found = 0;
    for (const auto* lbl : StemPipeline::stem_labels) {
        const auto path = std::filesystem::path{folder} / (std::string{lbl} + ".wav");
        if (std::filesystem::exists(path)) {
            sampler.set_stem(found, lbl, path.string());
            ++found;
            std::cout << "[stems] loaded: " << lbl << '\n';
        }
    }
    return found;
}

} // namespace analogno
