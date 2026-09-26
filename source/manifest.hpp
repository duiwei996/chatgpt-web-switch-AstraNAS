// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "model.hpp"
#include <string>
#include <vector>

bool parse_manifest_json(const std::string& text, std::vector<RemoteItem>& items, std::string& error);
bool parse_remote_filename(const std::string& filename, const std::string& path, std::uint64_t size, RemoteItem& out);
std::string title_id_hex(std::uint64_t title_id);
