// SPDX-License-Identifier: GPL-3.0-or-later
#include "download_cache.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
std::string basename_of(std::string path) {
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.pop_back();
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string sanitize_filename(std::string value) {
    for (char& c : value) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 32 || u == 0x7f || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '.')) value.pop_back();
    if (value.empty()) value = "download.bin";
    return value;
}

bool sync_file(std::FILE* file) {
    if (std::fflush(file) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return ::fsync(fileno(file)) == 0;
#endif
}
}

std::string remote_object_key(const AppConfig& config, const RemoteDirEntry& entry) {
    const std::string material = config.url + "\n" + config.username + "\n" + entry.path + "\n" +
                                 std::to_string(entry.size) + "\n" + entry.identity;
    return sha256_bytes(material.data(), material.size());
}

std::string remote_cache_filename(const AppConfig& config, const RemoteDirEntry& entry) {
    std::string name = sanitize_filename(basename_of(entry.name));
    const std::string suffix = "-" + remote_object_key(config, entry).substr(0, 16);
    const auto dot = name.find_last_of('.');
    std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
    std::string extension = dot == std::string::npos ? std::string{} : name.substr(dot);
    constexpr std::size_t kMaxName = 220;
    const std::size_t reserved = suffix.size() + extension.size();
    if (reserved >= kMaxName) extension.clear();
    const std::size_t stemLimit = kMaxName - suffix.size() - extension.size();
    if (stem.size() > stemLimit) stem.resize(stemLimit);
    if (stem.empty()) stem = "download";
    return stem + suffix + extension;
}

bool read_transfer_metadata(const std::string& path, TransferMetadata& metadata) {
    metadata = TransferMetadata{};
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    char buffer[256]{};
    while (std::fgets(buffer, sizeof(buffer), file)) {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.rfind("object_key=", 0) == 0) metadata.object_key = line.substr(11);
        else if (line.rfind("sha256=", 0) == 0) metadata.sha256 = line.substr(7);
    }
    const bool ok = std::ferror(file) == 0 && is_valid_sha256_hex(metadata.object_key) &&
                    (metadata.sha256.empty() || is_valid_sha256_hex(metadata.sha256));
    std::fclose(file);
    if (!ok) metadata = TransferMetadata{};
    return ok;
}

bool write_transfer_metadata(const std::string& path, const TransferMetadata& metadata,
                             std::string& error) {
    if (!is_valid_sha256_hex(metadata.object_key) ||
        (!metadata.sha256.empty() && !is_valid_sha256_hex(metadata.sha256))) {
        error = "invalid transfer metadata";
        return false;
    }
    const std::string temporary = path + ".new";
    std::FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) { error = "cannot create transfer metadata"; return false; }
    const std::string text = "object_key=" + normalize_sha256_hex(metadata.object_key) + "\n" +
                             (metadata.sha256.empty() ? std::string{} :
                              "sha256=" + normalize_sha256_hex(metadata.sha256) + "\n");
    bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size() && sync_file(file);
    if (std::fclose(file) != 0) ok = false;
    if (!ok) {
        std::remove(temporary.c_str());
        error = "failed to write transfer metadata";
        return false;
    }
    std::remove(path.c_str());
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        error = std::strerror(errno);
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

bool is_transfer_sidecar_name(const std::string& name) {
    const auto ends_with = [&](const char* suffix) {
        const std::size_t length = std::strlen(suffix);
        return name.size() >= length && name.compare(name.size() - length, length, suffix) == 0;
    };
    return ends_with(".astranas-part") || ends_with(".astranas-meta") ||
           ends_with(".astranas-meta.new");
}
