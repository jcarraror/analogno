#pragma once

// Minimal SF2 soundfont parser — extracts bank/program/name from the PHDR
// chunk only. Does not load sample data.

#include "web_state.hpp"

#include <algorithm>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace analogno {

// Navigates the RIFF structure to locate the 'pdta' LIST chunk and reads every
// preset header from the 'phdr' sub-chunk.
inline std::vector<WebPreset> read_sf2_presets(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "sf2_reader: cannot open " << path << '\n';
        return {};
    }

    const auto read_u32 = [&]() -> std::uint32_t {
        std::uint32_t v{};
        f.read(reinterpret_cast<char *>(&v), 4);
        return v;
    };
    const auto read_tag = [&]() -> std::uint32_t {
        return read_u32(); // treat as opaque 4-byte id
    };
    const auto tag_of = [](const char *s) -> std::uint32_t {
        std::uint32_t v{};
        std::memcpy(&v, s, 4);
        return v;
    };

    // RIFF header: "RIFF" <size> "sfbk"
    if (read_tag() != tag_of("RIFF")) return {};
    read_u32(); // riff size
    if (read_tag() != tag_of("sfbk")) return {};

    // Locate the 'pdta' LIST chunk by scanning top-level chunks.
    // INFO and sdta can be large; seek past them efficiently.
    std::streampos pdta_body_start{};
    std::uint32_t pdta_body_size{};

    while (f.good()) {
        const auto chunk_id   = read_tag();
        const auto chunk_size = read_u32();
        const auto chunk_body = f.tellg();

        if (chunk_id == tag_of("LIST")) {
            const auto list_type = read_tag();
            if (list_type == tag_of("pdta")) {
                pdta_body_start = f.tellg();
                pdta_body_size  = chunk_size - 4;
                break;
            }
        }

        // Skip to next chunk (sizes must be word-aligned).
        const auto skip = static_cast<std::streamoff>(chunk_size + (chunk_size & 1));
        f.seekg(static_cast<std::streamoff>(chunk_body) + skip);
    }

    if (!f.good() || pdta_body_size == 0) {
        std::cerr << "sf2_reader: pdta chunk not found in " << path << '\n';
        return {};
    }

    // Scan pdta sub-chunks for 'phdr'.
    f.seekg(pdta_body_start);
    const auto pdta_end = static_cast<std::streamoff>(pdta_body_start) +
                          static_cast<std::streamoff>(pdta_body_size);

    while (f.good() && static_cast<std::streamoff>(f.tellg()) < pdta_end - 8) {
        const auto sub_id   = read_tag();
        const auto sub_size = read_u32();
        const auto sub_body = f.tellg();

        if (sub_id == tag_of("phdr")) {
            constexpr auto record_size = std::size_t{38};
            const auto count = static_cast<std::size_t>(sub_size) / record_size;

            std::vector<WebPreset> presets;
            presets.reserve(count > 1 ? count - 1 : 0);

            for (std::size_t i = 0; i + 1 < count; ++i) { // last record is sentinel
                char name_buf[21]{};
                f.read(name_buf, 20);

                std::uint16_t prog{}, bank{};
                f.read(reinterpret_cast<char *>(&prog), 2);
                f.read(reinterpret_cast<char *>(&bank), 2);

                f.seekg(14, std::ios::cur); // skip bag/lib/genre/morph fields

                presets.push_back({
                    .bank    = static_cast<int>(bank),
                    .program = static_cast<int>(prog),
                    .name    = std::string{name_buf},
                });
            }

            std::sort(presets.begin(), presets.end(), [](const WebPreset &a, const WebPreset &b) {
                return a.bank != b.bank ? a.bank < b.bank : a.program < b.program;
            });
            return presets;
        }

        const auto skip = static_cast<std::streamoff>(sub_size + (sub_size & 1));
        f.seekg(static_cast<std::streamoff>(sub_body) + skip);
    }

    std::cerr << "sf2_reader: phdr chunk not found in " << path << '\n';
    return {};
}

} // namespace analogno
