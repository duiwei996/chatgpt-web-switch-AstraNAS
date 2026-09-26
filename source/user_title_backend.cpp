// SPDX-License-Identifier: GPL-3.0-or-later
#include "user_title_backend.hpp"
#include "package_inspect.hpp"
#include "title_backend/backend_provider.hpp"
#include "title_backend/install_session.hpp"

namespace astranas::user_backend {

PreflightDecision safe_batch_preflight_decision(const PackageInstallInfo& info) {
    bool hasActionable = false;
    for (const auto& content : info.contents) {
        if (content.relation == InstalledRelation::Downgrade) return PreflightDecision::Skip;
        if (content.relation == InstalledRelation::NotInstalled ||
            content.relation == InstalledRelation::Upgrade)
            hasActionable = true;
    }
    return hasActionable ? PreflightDecision::Install : PreflightDecision::Skip;
}

const char* install_storage_label(InstallStorage storage) {
    return storage == InstallStorage::NandUser ? "NAND USER" : "SD CARD";
}

Result install_title_package(
    const std::string& local_path,
    const ProgressCallback& progress,
    std::atomic_bool& cancel_requested,
    std::string& error,
    const InstallOptions& options) {
    const auto kind = detect_package_container(local_path);
    if (kind != PackageContainerKind::Nsp && kind != PackageContainerKind::Nsz &&
        kind != PackageContainerKind::Xci && kind != PackageContainerKind::Xcz) {
        error = "unsupported title package container";
        return Result::Unsupported;
    }
    return title_backend::InstallSession(title_backend::create_backend_provider(kind, options))
        .run(local_path, progress, cancel_requested, error);
}

} // namespace astranas::user_backend
