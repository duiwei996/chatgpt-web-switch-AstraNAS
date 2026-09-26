// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include "package_inspect.hpp"
#include "user_title_backend.hpp"

namespace astranas::title_backend { class PackageSource; }

enum class InstallCandidateKind {
    None,
    HomebrewNro,
    NintendoPackageNeedsBackend
};

enum class InstallResult {
    Success,
    Failed,
    Cancelled,
    Unsupported,
    Skipped,
    InstalledCleanupFailed
};

using InstallProgressCallback = std::function<bool(const std::string& stage, std::uint64_t current, std::uint64_t total)>;

InstallCandidateKind detect_install_candidate(const std::string& path);
const char* install_candidate_label(InstallCandidateKind kind);
InstallResult install_staged_file(const std::string& staged_path, std::string& installed_path,
                                  std::string& error, const InstallProgressCallback& progress = {},
                                  const astranas::user_backend::InstallOptions& options = {});
InstallResult install_package_source(const std::string& display_path, PackageContainerKind kind,
                                     astranas::title_backend::PackageSource& source,
                                     std::string& installed_path, std::string& error,
                                     const InstallProgressCallback& progress = {},
                                     const astranas::user_backend::InstallOptions& options = {});
