// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifdef __SWITCH__
#include <switch.h>
#include <cstdint>
#include <string>
#include <vector>

namespace astranas::installed {
struct TitleEntry {
    std::uint64_t application_id = 0;
    std::uint32_t base_version = 0;
    std::uint32_t patch_version = 0;
    NcmStorageId storage = NcmStorageId_None;
    std::uint64_t last_updated = 0;
    std::string name;
    std::string publisher;
    std::string display_version;
};
struct IconBitmap {
    std::vector<std::uint8_t> rgb;
    int width = 0;
    int height = 0;
};
bool load_cache(const std::string& cache_dir, std::vector<TitleEntry>& entries, std::string& error);
bool refresh(const std::string& cache_dir, std::vector<TitleEntry>& entries, std::string& error);
bool load_icon_rgb(const std::string& cache_dir, std::uint64_t application_id,
                   std::vector<std::uint8_t>& rgb, int& width, int& height, std::string& error);
std::string icon_path(const std::string& cache_dir, std::uint64_t application_id);
} // namespace astranas::installed
#endif
