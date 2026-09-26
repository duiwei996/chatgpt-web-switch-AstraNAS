// SPDX-License-Identifier: GPL-3.0-or-later
#include "library_match.hpp"
#include <unordered_map>
void compare_library(std::vector<RemoteItem>& remote, const std::vector<InstalledTitle>& installed) {
    std::unordered_map<std::uint64_t, std::uint32_t> versions;
    for (const auto& title : installed) versions[title.application_id] = title.version;
    for (auto& item : remote) {
        const auto it = versions.find(item.application_id);
        if (it == versions.end()) { item.state = CompareState::NotInstalled; item.installed_version = 0; continue; }
        item.installed_version = it->second;
        if (item.version == 0 || it->second == 0) item.state = CompareState::Unknown;
        else if (item.version == it->second) item.state = CompareState::Same;
        else if (item.version > it->second) item.state = CompareState::RemoteNewer;
        else item.state = CompareState::LocalNewer;
    }
}
