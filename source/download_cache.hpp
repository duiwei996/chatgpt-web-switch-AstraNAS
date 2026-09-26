// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "model.hpp"

#include <string>

struct TransferMetadata {
    std::string object_key;
    std::string sha256;
};

std::string remote_object_key(const AppConfig& config, const RemoteDirEntry& entry);
std::string remote_cache_filename(const AppConfig& config, const RemoteDirEntry& entry);
bool read_transfer_metadata(const std::string& path, TransferMetadata& metadata);
bool write_transfer_metadata(const std::string& path, const TransferMetadata& metadata,
                             std::string& error);
bool is_transfer_sidecar_name(const std::string& name);
