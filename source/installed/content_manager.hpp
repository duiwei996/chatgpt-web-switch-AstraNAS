// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifdef __SWITCH__

#include <switch.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace astranas::installed {

struct ManagedContentMeta {
    std::uint64_t id = 0;
    std::uint32_t version = 0;
    NcmContentMetaType type = NcmContentMetaType_Unknown;
    NcmStorageId storage = NcmStorageId_None;
};

struct RemovalSummary {
    std::size_t meta_records = 0;
    std::size_t content_files = 0;
    std::string warning;
};

bool list_dlc(std::uint64_t application_id, std::vector<ManagedContentMeta>& entries,
              std::string& error);
bool remove_update(std::uint64_t application_id, RemovalSummary& summary, std::string& error);
bool remove_all_dlc(std::uint64_t application_id, RemovalSummary& summary, std::string& error);
bool remove_dlc(std::uint64_t application_id, const ManagedContentMeta& dlc,
                RemovalSummary& summary, std::string& error);

} // namespace astranas::installed
#endif
