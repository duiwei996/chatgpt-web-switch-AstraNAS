// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace astranas::user_backend {

enum class Result {
    Success,
    Failed,
    Cancelled,
    Unsupported,
    Skipped,
};

struct Progress {
    std::uint64_t current = 0;
    std::uint64_t total = 0;
    std::string stage;
};

enum class InstallStorage {
    SdCard,
    NandUser,
};

enum class ContentKind {
    Application,
    Patch,
    AddOnContent,
    DataPatch,
    Unknown,
};

enum class InstalledRelation {
    NotInstalled,
    Upgrade,
    SameVersion,
    Downgrade,
};

struct PackageContentInfo {
    std::uint64_t title_id = 0;
    std::uint64_t application_id = 0;
    std::uint64_t install_size = 0;
    std::uint32_t version = 0;
    std::uint32_t required_system_version = 0;
    std::uint32_t installed_version = 0;
    ContentKind kind = ContentKind::Unknown;
    InstalledRelation relation = InstalledRelation::NotInstalled;
};

struct PackageInstallInfo {
    std::string container;
    std::string title_name;
    std::string publisher;
    std::string display_version;
    bool title_metadata_from_installed_content = false;
    std::uint64_t source_size = 0;
    std::uint32_t package_file_count = 0;
    std::uint32_t ticket_count = 0;
    std::uint32_t certificate_count = 0;
    std::uint64_t required_space = 0;
    std::uint64_t force_required_space = 0;
    std::uint64_t free_space = 0;
    InstallStorage storage = InstallStorage::SdCard;
    std::vector<PackageContentInfo> contents;
};

enum class PreflightDecision {
    Install,
    ForceInstall,
    Skip,
    Cancel,
};

using PreflightCallback = std::function<PreflightDecision(const PackageInstallInfo&)>;
using InvalidNcaCallback = std::function<bool(const std::string& content_id)>;

// Non-interactive batches install new content/upgrades, but never silently
// reinstall an all-current package or apply any downgrade.
PreflightDecision safe_batch_preflight_decision(const PackageInstallInfo& info);

struct InstallOptions {
    bool ignore_required_firmware = false;
    bool validate_nca = true;
    bool verify_nca_content_hash = false;
    InstallStorage storage = InstallStorage::SdCard;
    PreflightCallback preflight;
    InvalidNcaCallback invalid_nca;
};

const char* install_storage_label(InstallStorage storage);

using ProgressCallback = std::function<void(const Progress&)>;

// User-maintained integration point for protected Nintendo title packages.
// AstraNAS owns networking, cache lifecycle, queueing, UI and post-success
// 这个兼容入口接收本地路径；v1.1.0 的网络直装则通过 PackageSource
// 把数据送入同一 provider，缓存生命周期、队列、UI 与成功后清理仍由 AstraNAS 负责。
Result install_title_package(
    const std::string& local_path,
    const ProgressCallback& progress,
    std::atomic_bool& cancel_requested,
    std::string& error,
    const InstallOptions& options = {});

} // namespace astranas::user_backend
