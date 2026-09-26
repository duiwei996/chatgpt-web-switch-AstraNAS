// SPDX-License-Identifier: GPL-3.0-or-later
#include "package_source.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <iterator>
#include <limits>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>

namespace astranas::title_backend {
namespace {
struct SplitPattern {
    bool matched = false;
    std::string prefix;
    std::size_t width = 0;
    std::uint64_t index = 0;
};

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool all_digits(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
}

bool parse_index(const std::string& digits, std::uint64_t& index) {
    if (!all_digits(digits) || digits.size() > 9) return false;
    index = 0;
    for (const char c : digits) index = index * 10 + static_cast<unsigned>(c - '0');
    return true;
}

SplitPattern split_pattern(const std::string& path) {
    const std::string lower = lower_copy(path);
    const auto slash = lower.find_last_of("/\\");
    const std::size_t name_start = slash == std::string::npos ? 0 : slash + 1;

    for (const char* marker : {".nsp.part", ".nsz.part", ".xci.part", ".xcz.part",
                               ".nsp.", ".nsz.", ".xci.", ".xcz."}) {
        const std::string token(marker);
        const auto pos = lower.rfind(token);
        if (pos == std::string::npos || pos < name_start) continue;
        const std::string digits = lower.substr(pos + token.size());
        std::uint64_t index = 0;
        if (parse_index(digits, index))
            return {true, path.substr(0, pos + token.size()), digits.size(), index};
    }

    const auto dot = lower.find_last_of('.');
    if (dot != std::string::npos && dot >= name_start && dot + 3 < lower.size()) {
        const std::string tag = lower.substr(dot + 1, 2);
        const std::string digits = lower.substr(dot + 3);
        std::uint64_t index = 0;
        if ((tag == "ns" || tag == "xc") && parse_index(digits, index))
            return {true, path.substr(0, dot + 3), digits.size(), index};
    }
    return {};
}

bool regular_file_size(const std::string& path, std::uint64_t& size) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode) || st.st_size < 0) return false;
    size = static_cast<std::uint64_t>(st.st_size);
    return true;
}

bool directory_parts(const std::string& path, std::vector<std::string>& parts,
                     std::string& error) {
    DIR* dir = opendir(path.c_str());
    if (!dir) { error = "cannot open split package directory: " + std::string(std::strerror(errno)); return false; }
    std::vector<std::pair<std::uint64_t, std::string>> indexed;
    while (dirent* ent = readdir(dir)) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::uint64_t index = 0;
        if (!parse_index(name, index)) continue;
        const std::string child = path + (path.empty() || path.back() == '/' ? "" : "/") + name;
        std::uint64_t ignored = 0;
        if (regular_file_size(child, ignored)) indexed.emplace_back(index, child);
    }
    closedir(dir);
    if (indexed.empty()) { error = "split package directory contains no numeric parts"; return false; }
    std::sort(indexed.begin(), indexed.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });
    for (std::size_t i = 0; i < indexed.size(); ++i) {
        if (indexed[i].first != i) {
            error = "split package has a missing or duplicate numeric part";
            return false;
        }
        parts.push_back(indexed[i].second);
    }
    return true;
}

bool seek_file(std::FILE* fp, std::uint64_t offset) {
#ifdef _WIN32
    return offset <= static_cast<std::uint64_t>(std::numeric_limits<__int64>::max()) &&
           _fseeki64(fp, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return offset <= static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) &&
           fseeko(fp, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

bool opened_file_size(std::FILE* fp, std::uint64_t& size) {
#ifdef _WIN32
    if (_fseeki64(fp, 0, SEEK_END) != 0) return false;
    const __int64 end = _ftelli64(fp);
#else
    if (fseeko(fp, 0, SEEK_END) != 0) return false;
    const off_t end = ftello(fp);
#endif
    if (end < 0 || !seek_file(fp, 0)) return false;
    size = static_cast<std::uint64_t>(end);
    return true;
}
} // namespace

bool PackageSource::read_at(std::uint64_t offset, void* buffer, std::size_t size,
                            std::size_t& actual, std::string& error) {
    if (!seek(offset, error)) { actual = 0; return false; }
    return read(buffer, size, actual, error);
}

bool is_split_package_first_part(const std::string& path) {
    const auto pattern = split_pattern(path);
    return pattern.matched && pattern.index == 0;
}

bool describe_split_package_part(const std::string& path, SplitPackagePartInfo& info) {
    const auto pattern = split_pattern(path);
    if (!pattern.matched) { info = {}; return false; }
    info.prefix = pattern.prefix;
    info.width = pattern.width;
    info.index = pattern.index;
    return true;
}

bool is_split_package_later_part(const std::string& path) {
    const auto pattern = split_pattern(path);
    return pattern.matched && pattern.index != 0;
}

bool resolve_package_parts(const std::string& path, std::vector<std::string>& parts,
                           std::string& error) {
    parts.clear();
    error.clear();
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        error = "cannot open package source: " + std::string(std::strerror(errno));
        return false;
    }
    if (S_ISDIR(st.st_mode)) return directory_parts(path, parts, error);

    const auto pattern = split_pattern(path);
    if (!pattern.matched) { parts.push_back(path); return true; }
    if (pattern.index != 0) {
        error = "select the first split package part";
        return false;
    }

    const auto slash = path.find_last_of("/\\");
    const std::string parent = slash == std::string::npos ? "." : path.substr(0, slash);
    const std::string expected_prefix = lower_copy(pattern.prefix);
    DIR* dir = opendir(parent.empty() ? "." : parent.c_str());
    if (!dir) {
        error = "cannot list split package directory: " + std::string(std::strerror(errno));
        return false;
    }
    std::vector<std::pair<std::uint64_t, std::string>> indexed;
    while (dirent* ent = readdir(dir)) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        const std::string candidate = slash == std::string::npos ? name :
            parent + path.substr(slash, 1) + name;
        const auto candidate_pattern = split_pattern(candidate);
        std::uint64_t ignored = 0;
        if (!candidate_pattern.matched ||
            lower_copy(candidate_pattern.prefix) != expected_prefix ||
            candidate_pattern.width != pattern.width ||
            !regular_file_size(candidate, ignored))
            continue;
        indexed.emplace_back(candidate_pattern.index, candidate);
    }
    closedir(dir);
    if (indexed.empty()) { error = "split package first part is missing"; return false; }
    if (indexed.size() > 10000) { error = "split package contains too many parts"; return false; }
    std::sort(indexed.begin(), indexed.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });
    for (std::size_t i = 0; i < indexed.size(); ++i) {
        if (indexed[i].first != i) {
            error = "split package has a missing or duplicate part";
            return false;
        }
        parts.push_back(indexed[i].second);
    }
    return true;
}

void FilePackageSource::close() {
    for (auto& part : parts_) if (part.file) std::fclose(static_cast<std::FILE*>(part.file));
    parts_.clear();
    part_paths_.clear();
    size_ = 0;
    position_ = 0;
    path_.clear();
}
FilePackageSource::~FilePackageSource() { close(); }
bool FilePackageSource::open(const std::string& path, std::string& error) {
    close();
    if (!resolve_package_parts(path, part_paths_, error)) return false;
    for (const auto& part_path : part_paths_) {
        std::FILE* fp = std::fopen(part_path.c_str(), "rb");
        if (!fp) { error = "cannot open package part: " + part_path; close(); return false; }
        std::uint64_t part_size = 0;
        if (!opened_file_size(fp, part_size)) {
            std::fclose(fp); error = "cannot determine package part size"; close(); return false;
        }
        if (part_size > std::numeric_limits<std::uint64_t>::max() - size_) {
            std::fclose(fp); error = "split package size overflow"; close(); return false;
        }
        parts_.push_back({fp, size_, part_size});
        size_ += part_size;
    }
    if (parts_.empty() || size_ == 0) { error = "package source is empty"; close(); return false; }
    path_ = path;
    return true;
}
bool FilePackageSource::read(void* buffer, std::size_t size, std::size_t& actual, std::string& error) {
    actual = 0;
    if (parts_.empty()) { error = "package source is not open"; return false; }
    auto* output = static_cast<unsigned char*>(buffer);
    while (actual < size && position_ < size_) {
        const auto it = std::upper_bound(parts_.begin(), parts_.end(), position_,
            [](std::uint64_t position, const Part& part) { return position < part.offset; });
        const auto selected = it == parts_.begin() ? parts_.begin() : std::prev(it);
        const std::uint64_t within = position_ - selected->offset;
        if (within >= selected->size) { error = "split package part lookup failed"; return false; }
        const std::uint64_t available = selected->size - within;
        const std::size_t request = static_cast<std::size_t>(std::min<std::uint64_t>(size - actual, available));
        auto* fp = static_cast<std::FILE*>(selected->file);
        if (!seek_file(fp, within)) { error = "package part seek failed"; return false; }
        const std::size_t got = std::fread(output + actual, 1, request, fp);
        actual += got;
        position_ += got;
        if (got != request) {
            error = std::ferror(fp) ? "package part read failed" : "package part is truncated";
            return false;
        }
    }
    return true;
}
bool FilePackageSource::seek(std::uint64_t offset, std::string& error) {
    if (parts_.empty() || offset > size_) { error = "invalid package seek"; return false; }
    position_ = offset;
    return true;
}
} // namespace astranas::title_backend
