// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum class CompareState { Unknown, NotInstalled, Same, RemoteNewer, LocalNewer };

struct InstalledTitle {
    std::uint64_t application_id = 0;
    std::uint32_t version = 0;
    std::string name;
    bool has_patch = false;
};

struct RemoteItem {
    std::uint64_t application_id = 0;
    std::uint32_t version = 0;
    std::uint64_t size = 0;
    std::string name;
    std::string path;
    std::string content_type = "application";
    std::string sha256;
    CompareState state = CompareState::Unknown;
    std::uint32_t installed_version = 0;
};

struct RemoteDirEntry {
    std::string name;
    std::string path;
    // Stable remote revision (ETag/Last-Modified/SMB mtime) used to bind
    // resumable partial files to the exact object that produced them.
    std::string identity;
    std::uint64_t size = 0;
    bool is_dir = false;
};

inline const char* compare_state_text(CompareState state) {
    switch (state) {
        case CompareState::NotInstalled: return "NOT INSTALLED";
        case CompareState::Same: return "SAME";
        case CompareState::RemoteNewer: return "UPDATE";
        case CompareState::LocalNewer: return "LOCAL NEWER";
        default: return "UNKNOWN";
    }
}
