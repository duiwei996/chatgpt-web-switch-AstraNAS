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

bool is_utf8_continuation(unsigned char value) {
    return (value & 0xC0) == 0x80;
}

std::string sanitize_filename(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size();) {
        const unsigned char c0 = static_cast<unsigned char>(value[i]);
        if (c0 < 0x80) {
            const char c = static_cast<char>(c0);
            if (c0 < 32 || c0 == 0x7f || c == '/' || c == '\\' || c == ':' || c == '*' ||
                c == '?' || c == '"' || c == '<' || c == '>' || c == '|') out.push_back('_');
            else out.push_back(c);
            ++i;
            continue;
        }

        std::size_t width = 0;
        bool valid = false;
        if (c0 >= 0xC2 && c0 <= 0xDF) {
            width = 2;
            valid = i + width <= value.size() &&
                    is_utf8_continuation(static_cast<unsigned char>(value[i + 1]));
        } else if (c0 >= 0xE0 && c0 <= 0xEF) {
            width = 3;
            valid = i + width <= value.size() &&
                    is_utf8_continuation(static_cast<unsigned char>(value[i + 1])) &&
                    is_utf8_continuation(static_cast<unsigned char>(value[i + 2]));
            if (valid && c0 == 0xE0) valid = static_cast<unsigned char>(value[i + 1]) >= 0xA0;
            if (valid && c0 == 0xED) valid = static_cast<unsigned char>(value[i + 1]) < 0xA0;
        } else if (c0 >= 0xF0 && c0 <= 0xF4) {
            width = 4;
            valid = i + width <= value.size() &&
                    is_utf8_continuation(static_cast<unsigned char>(value[i + 1])) &&
                    is_utf8_continuation(static_cast<unsigned char>(value[i + 2])) &&
                    is_utf8_continuation(static_cast<unsigned char>(value[i + 3]));
            if (valid && c0 == 0xF0) valid = static_cast<unsigned char>(value[i + 1]) >= 0x90;
            if (valid && c0 == 0xF4) valid = static_cast<unsigned char>(value[i + 1]) <= 0x8F;
        }

        if (!valid) {
            out.push_back('_');
            ++i;
            continue;
        }
        out.append(value, i, width);
        i += width;
    }

    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    if (out.empty()) out = "download.bin";
    return out;
}

std::size_t utf8_prefix_bytes(const std::string& value, std::size_t max_bytes) {
    std::size_t i = 0;
    std::size_t accepted = 0;
    while (i < value.size() && i < max_bytes) {
        const unsigned char c0 = static_cast<unsigned char>(value[i]);
        std::size_t width = 1;
        if (c0 < 0x80) width = 1;
        else if (c0 >= 0xC2 && c0 <= 0xDF) width = 2;
        else if (c0 >= 0xE0 && c0 <= 0xEF) width = 3;
        else if (c0 >= 0xF0 && c0 <= 0xF4) width = 4;
        else break;
        if (i + width > value.size() || i + width > max_bytes) break;
        accepted = i + width;
        i += width;
    }
    return accepted;
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

std::string remote_cache_filename(const AppConfig&, const RemoteDirEntry& entry) {
    // User-visible downloads keep the NAS filename. Remote identity belongs in
    // the .astranas-meta sidecar, not in a hash suffix on the visible file.
    std::string name = sanitize_filename(basename_of(entry.name));
    const auto dot = name.find_last_of('.');
    std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
    std::string extension = dot == std::string::npos ? std::string{} : name.substr(dot);

    constexpr std::size_t kMaxNameBytes = 220;
    if (extension.size() >= kMaxNameBytes) extension.clear();
    const std::size_t stemLimit = kMaxNameBytes - extension.size();
    if (stem.size() > stemLimit) stem.resize(utf8_prefix_bytes(stem, stemLimit));
    if (stem.empty()) stem = "download";
    return stem + extension;
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
