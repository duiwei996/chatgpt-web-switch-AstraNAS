// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifdef __SWITCH__
#include <cstdint>
#include <string>

namespace astranas::app {
std::string format_size(std::uint64_t bytes);
std::string format_speed(double mib_per_sec);
std::string basename_of(std::string path);
std::uint64_t local_file_size(const std::string& path);
std::string lower_copy(std::string value);
void append_debug_log(const std::string& context, const std::string& detail);
std::string friendly_error(const std::string& action, const std::string& raw);
std::string trim_remote(std::string path);
bool remote_has_unsafe_component(const std::string& path);
bool remote_path_within(const std::string& root, const std::string& path);
std::string remote_parent_within(const std::string& root, const std::string& current);
std::string local_parent_within(std::string root, std::string current);
std::string title_id_text(std::uint64_t id);
}
#endif
