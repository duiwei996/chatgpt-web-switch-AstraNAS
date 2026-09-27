// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct LocalEntry {
    std::string name;
    std::string path;
    std::uint64_t size = 0;
    bool is_dir = false;
};

bool local_path_exists(const std::string& path);
bool local_mkdir_p(const std::string& path);
bool list_local_dir(const std::string& path, std::vector<LocalEntry>& entries, std::string& error);
std::string local_join_path(const std::string& parent, const std::string& child);
std::string local_parent_directory(const std::string& path);
std::string local_path_diagnostic(const std::string& path);
std::string local_parent_within(const std::string& root, const std::string& current);
bool local_path_is_within(const std::string& root, const std::string& path);
bool delete_local_entry(const std::string& root, const LocalEntry& entry, std::string& error);
bool clear_local_directory(const std::string& root, std::string& error);
bool copy_local_file(const std::string& source, const std::string& destination, std::string& error);

bool move_local_entry(const std::string& source_root, const std::string& destination_root,
                      const LocalEntry& entry, const std::string& destination_directory,
                      std::string& destination_path, std::string& error);
