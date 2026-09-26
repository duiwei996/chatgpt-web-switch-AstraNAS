// SPDX-License-Identifier: GPL-3.0-or-later
#include "installer.hpp"
#include "local_fs.hpp"
#include "package_inspect.hpp"
#include "user_title_backend.hpp"
#include "title_backend/backend_provider.hpp"
#include "title_backend/install_session.hpp"
#include "title_backend/package_source.hpp"
#include <atomic>

namespace {
std::string basename_of(std::string path) {
    while (!path.empty() && path.back() == '/') path.pop_back();
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
std::string stem_of(const std::string& name) {
    const auto dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}
}

InstallCandidateKind detect_install_candidate(const std::string& path) {
    const auto kind = detect_package_container(path);
    if (kind == PackageContainerKind::HomebrewNro) return InstallCandidateKind::HomebrewNro;
    if (kind == PackageContainerKind::Nsp || kind == PackageContainerKind::Nsz ||
        kind == PackageContainerKind::Xci || kind == PackageContainerKind::Xcz)
        return InstallCandidateKind::NintendoPackageNeedsBackend;
    return InstallCandidateKind::None;
}

const char* install_candidate_label(InstallCandidateKind kind) {
    switch (kind) {
        case InstallCandidateKind::HomebrewNro: return "NRO";
        case InstallCandidateKind::NintendoPackageNeedsBackend: return "PKG";
        default: return "";
    }
}

InstallResult install_staged_file(const std::string& staged_path, std::string& installed_path,
                                  std::string& error, const InstallProgressCallback& progress,
                                  const astranas::user_backend::InstallOptions& options) {
    installed_path.clear();
    const auto kind = detect_install_candidate(staged_path);
    if (kind == InstallCandidateKind::NintendoPackageNeedsBackend) {
        std::atomic_bool cancel_requested{false};
        const astranas::user_backend::ProgressCallback backend_progress = [&](const astranas::user_backend::Progress& p) {
            if (progress && !progress(p.stage.empty() ? "User title backend" : p.stage, p.current, p.total))
                cancel_requested.store(true);
        };
        const auto rc = astranas::user_backend::install_title_package(
            staged_path, backend_progress, cancel_requested, error, options);
        switch (rc) {
            case astranas::user_backend::Result::Success:
                installed_path = astranas::user_backend::install_storage_label(options.storage);
                return InstallResult::Success;
            case astranas::user_backend::Result::Cancelled:
                if (error.empty()) error = "用户 Title 安装后端已取消";
                return InstallResult::Cancelled;
            case astranas::user_backend::Result::Unsupported:
                if (error.empty()) error = "用户 Title 安装 provider 不可用";
                return InstallResult::Unsupported;
            case astranas::user_backend::Result::Skipped:
                return InstallResult::Skipped;
            default:
                if (error.empty()) error = "用户 Title 安装后端失败";
                return InstallResult::Failed;
        }
    }
    if (kind != InstallCandidateKind::HomebrewNro) {
        error = "unsupported install file type";
        return InstallResult::Unsupported;
    }

    const std::string name = basename_of(staged_path);
    std::string folder = stem_of(name);
    if (folder.empty()) folder = "AstraNAS-installed";
    const std::string target_dir = "sdmc:/switch/" + folder;
    if (!local_mkdir_p(target_dir)) { error = "cannot create target directory"; return InstallResult::Failed; }
    installed_path = local_join_path(target_dir, name);
    if (!copy_local_file(staged_path, installed_path, error)) return InstallResult::Failed;
    return InstallResult::Success;
}

InstallResult install_package_source(const std::string& display_path, PackageContainerKind kind,
                                     astranas::title_backend::PackageSource& source,
                                     std::string& installed_path, std::string& error,
                                     const InstallProgressCallback& progress,
                                     const astranas::user_backend::InstallOptions& options) {
    installed_path.clear();
    if (kind != PackageContainerKind::Nsp && kind != PackageContainerKind::Nsz &&
        kind != PackageContainerKind::Xci && kind != PackageContainerKind::Xcz) {
        error = "不支持该网络直装容器格式";
        return InstallResult::Unsupported;
    }

    PackageInspection inspection;
    if (!inspect_package_source(display_path, kind, source, inspection, error))
        return InstallResult::Failed;

    std::atomic_bool cancel_requested{false};
    const astranas::user_backend::ProgressCallback backend_progress = [&](const astranas::user_backend::Progress& p) {
        if (progress && !progress(p.stage.empty() ? "User title backend" : p.stage, p.current, p.total))
            cancel_requested.store(true);
    };
    auto provider = astranas::title_backend::create_backend_provider(kind, options);
    const auto rc = astranas::title_backend::InstallSession(std::move(provider))
        .run(source, inspection, backend_progress, cancel_requested, error);
    switch (rc) {
        case astranas::user_backend::Result::Success:
            installed_path = astranas::user_backend::install_storage_label(options.storage);
            return InstallResult::Success;
        case astranas::user_backend::Result::Cancelled:
            if (error.empty()) error = "用户 Title 安装后端已取消";
            return InstallResult::Cancelled;
        case astranas::user_backend::Result::Unsupported:
            if (error.empty()) error = "用户 Title 安装 provider 不可用";
            return InstallResult::Unsupported;
        case astranas::user_backend::Result::Skipped:
            return InstallResult::Skipped;
        default:
            if (error.empty()) error = "用户 Title 安装后端失败";
            return InstallResult::Failed;
    }
}
