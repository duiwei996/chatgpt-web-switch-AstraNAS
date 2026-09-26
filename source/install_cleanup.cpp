// SPDX-License-Identifier: GPL-3.0-or-later
#include "install_cleanup.hpp"

#include "local_fs.hpp"
#include "title_backend/package_source.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <vector>

namespace {
std::string basename_of(std::string path) {
    while (!path.empty() && path.back() == '/') path.pop_back();
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string parent_of(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return {};
    const auto mount = path.find(":/");
    if (mount != std::string::npos && slash == mount + 1) return path.substr(0, slash + 1);
    return path.substr(0, slash);
}

bool remove_exact_part(const std::string& part, std::string& error) {
    if (std::remove(part.c_str()) != 0) {
        error = "cannot remove " + basename_of(part) + ": " + std::string(std::strerror(errno));
        return false;
    }
    std::remove((part + ".astranas-meta").c_str());
    return true;
}
}

bool remove_installed_package_source(const std::string& path, std::string& error) {
    error.clear();
    const std::string parent = parent_of(path);
    if (parent.empty() || path == parent) {
        error = "refusing cleanup without an exact parent directory";
        return false;
    }

    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) { error = std::strerror(errno); return false; }
    if (S_ISDIR(st.st_mode)) {
        std::vector<std::string> parts;
        if (!astranas::title_backend::resolve_package_parts(path, parts, error)) return false;
        for (const auto& part : parts) {
            if (parent_of(part) != path || !local_path_is_within(path, part)) {
                error = "split package part escaped selected package directory";
                return false;
            }
        }
        for (const auto& part : parts) if (!remove_exact_part(part, error)) return false;

        // A split-package directory may contain user files that are unrelated to
        // the numbered parts. Remove the directory only when it is now empty.
        if (std::remove(path.c_str()) != 0 && errno != ENOTEMPTY && errno != EEXIST) {
            error = "parts removed, but package directory cleanup failed: " +
                    std::string(std::strerror(errno));
            return false;
        }
        std::remove((path + ".astranas-meta").c_str());
        return true;
    }

    std::vector<std::string> parts;
    if (!astranas::title_backend::resolve_package_parts(path, parts, error)) return false;
    for (const auto& part : parts) {
        if (parent_of(part) != parent || !local_path_is_within(parent, part)) {
            error = "split package part escaped selected source directory";
            return false;
        }
    }
    for (const auto& part : parts) if (!remove_exact_part(part, error)) return false;
    return true;
}
