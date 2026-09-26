// SPDX-License-Identifier: GPL-3.0-or-later
#include "backend_provider.hpp"
#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <sstream>
#include <string>

#ifdef __SWITCH__
#include "bridge/install_hooks.hpp"
#include "install/install_nsp.hpp"
#include "install/install_xci.hpp"
#include "install/sdmc_nsp.hpp"
#include "install/sdmc_xci.hpp"
#include "util/config.hpp"
#include "util/title_util.hpp"
#include "util/util.hpp"
#include <switch.h>
#endif

namespace astranas::title_backend {
namespace {
#ifdef __SWITCH__
NcmStorageId storage_id_for(user_backend::InstallStorage storage) {
    return storage == user_backend::InstallStorage::NandUser
        ? NcmStorageId_BuiltInUser : NcmStorageId_SdCard;
}

user_backend::ContentKind content_kind_for(NcmContentMetaType type) {
    switch (type) {
        case NcmContentMetaType_Application: return user_backend::ContentKind::Application;
        case NcmContentMetaType_Patch: return user_backend::ContentKind::Patch;
        case NcmContentMetaType_AddOnContent: return user_backend::ContentKind::AddOnContent;
        case NcmContentMetaType_DataPatch: return user_backend::ContentKind::DataPatch;
        default: return user_backend::ContentKind::Unknown;
    }
}

std::string bounded_text(const char* value, std::size_t capacity) {
    if (!value || capacity == 0) return {};
    std::size_t length = 0;
    while (length < capacity && value[length] != '\0') ++length;
    return std::string(value, length);
}

void load_installed_title_metadata(u64 applicationId,
                                   user_backend::PackageInstallInfo& info) {
    auto control = std::make_unique<NsApplicationControlData>();
    u64 actualSize = 0;
    if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage,
                                             applicationId, control.get(), sizeof(*control),
                                             &actualSize)) ||
        actualSize < sizeof(NacpStruct))
        return;
    NacpLanguageEntry* language = nullptr;
    if (R_SUCCEEDED(nsGetApplicationDesiredLanguage(&control->nacp, &language)) && language) {
        info.title_name = bounded_text(language->name, sizeof(language->name));
        info.publisher = bounded_text(language->author, sizeof(language->author));
    }
    info.display_version = bounded_text(control->nacp.display_version,
                                        sizeof(control->nacp.display_version));
    info.title_metadata_from_installed_content = !info.title_name.empty() ||
        !info.publisher.empty() || !info.display_version.empty();
}

enum class InstalledVersionLookup { Found, NotFound, Failed };

InstalledVersionLookup latest_installed_version(u64 applicationId, u64 titleId,
                                                NcmContentMetaType type, u32& version,
                                                std::string& error) {
    s32 count = 0;
    const Result countRc = nsCountApplicationContentMeta(applicationId, &count);
    if (countRc == MAKERESULT(16, 2)) return InstalledVersionLookup::NotFound;
    if (R_FAILED(countRc)) {
        std::ostringstream out;
        out << "failed to query installed application metadata (0x" << std::hex << countRc << ')';
        error = out.str();
        return InstalledVersionLookup::Failed;
    }
    if (count <= 0) return InstalledVersionLookup::NotFound;
    if (count > 4096) {
        error = "installed application metadata contains too many records";
        return InstalledVersionLookup::Failed;
    }
    std::vector<ContentStorageRecord> records(static_cast<std::size_t>(count));
    u32 written = 0;
    const Result listRc = nsListApplicationRecordContentMeta(
        0, applicationId, records.data(), static_cast<u32>(records.size()), &written);
    if (R_FAILED(listRc) || written > records.size()) {
        std::ostringstream out;
        out << "failed to read installed application metadata (0x" << std::hex << listRc << ')';
        error = out.str();
        return InstalledVersionLookup::Failed;
    }
    bool found = false;
    for (u32 i = 0; i < written; ++i) {
        const auto& key = records[i].metaRecord;
        if (key.id != titleId || key.type != type ||
            key.install_type != NcmContentInstallType_Full) continue;
        version = found ? std::max(version, key.version) : key.version;
        found = true;
    }
    return found ? InstalledVersionLookup::Found : InstalledVersionLookup::NotFound;
}

bool describe_content(const tin::install::PreparedContentInfo& prepared,
                      user_backend::PackageContentInfo& info, std::string& error) {
    info = {};
    info.title_id = prepared.titleId;
    info.application_id = prepared.applicationId;
    info.install_size = prepared.installSize;
    info.version = prepared.version;
    info.required_system_version = prepared.requiredSystemVersion;
    info.kind = content_kind_for(prepared.type);

    const auto lookup = latest_installed_version(prepared.applicationId, prepared.titleId,
                                                 prepared.type, info.installed_version, error);
    if (lookup == InstalledVersionLookup::Failed) return false;
    const bool installed = lookup == InstalledVersionLookup::Found;
    if (!installed) info.relation = user_backend::InstalledRelation::NotInstalled;
    else if (prepared.version > info.installed_version) info.relation = user_backend::InstalledRelation::Upgrade;
    else if (prepared.version == info.installed_version) info.relation = user_backend::InstalledRelation::SameVersion;
    else info.relation = user_backend::InstalledRelation::Downgrade;
    return true;
}
#endif

class AtmoXLProvider final : public BackendProvider {
public:
    AtmoXLProvider(PackageContainerKind kind, const user_backend::InstallOptions& options)
        : kind_(kind), options_(options) {}

    bool supports(PackageContainerKind kind) const override {
        return kind == PackageContainerKind::Nsp || kind == PackageContainerKind::Nsz ||
               kind == PackageContainerKind::Xci || kind == PackageContainerKind::Xcz;
    }

    bool begin_session(const PackageInspection& inspection, std::string& error) override {
#ifdef __SWITCH__
        try {
            inspection_ = inspection;
            inst::config::ignoreReqVers = options_.ignore_required_firmware;
            inst::config::validateNCAs = options_.validate_nca;
            inst::config::verifyNcaContentHashes = options_.verify_nca_content_hash;
            inst::util::initInstallServices();
            services_initialized_ = true;
            appletLockExit();
            exit_locked_ = true;
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
#else
        (void)inspection;
        error = "AtmoXL installer requires a Switch/libnx build";
        return false;
#endif
    }

    bool write_content(PackageSource& input, const user_backend::ProgressCallback& progress,
                       std::atomic_bool& cancel, std::string& error) override {
#ifdef __SWITCH__
        if (cancel.load()) { error = "installation cancelled"; return false; }
        input.set_cancel_flag(&cancel);
        struct CancelFlagReset {
            PackageSource& source;
            ~CancelFlagReset() { source.set_cancel_flag(nullptr); }
        } cancel_flag_reset{input};
        try {
            const NcmStorageId destination = storage_id_for(options_.storage);
            progress_ = &progress;
            cancel_ = &cancel;
            astranas::atmo_xl_bridge::set_install_hooks(&AtmoXLProvider::forward_progress,
                                                        &AtmoXLProvider::forward_invalid_nca,
                                                        this, &cancel);
            if (progress) progress({0, input.size(), "AtmoXL Prepare"});
            auto packageSource = std::shared_ptr<PackageSource>(&input, [](PackageSource*) {});
            if (kind_ == PackageContainerKind::Nsp || kind_ == PackageContainerKind::Nsz) {
                auto source = std::make_shared<tin::install::nsp::SDMCNSP>(packageSource);
                task_ = std::make_unique<tin::install::nsp::NSPInstall>(destination, inst::config::ignoreReqVers, source);
            } else if (kind_ == PackageContainerKind::Xci || kind_ == PackageContainerKind::Xcz) {
                auto source = std::make_shared<tin::install::xci::SDMCXCI>(packageSource);
                task_ = std::make_unique<tin::install::xci::XCIInstallTask>(destination, inst::config::ignoreReqVers, source);
            } else {
                error = "unsupported AtmoXL package container";
                return false;
            }
            task_->Prepare();
            const auto storageInfo = task_->GetPreparedStorageInfo();
            if (options_.preflight) {
                user_backend::PackageInstallInfo info{};
                info.container = package_container_label(kind_);
                info.source_size = input.size();
                info.package_file_count = inspection_.file_count;
                info.ticket_count = inspection_.ticket_count;
                info.certificate_count = inspection_.certificate_count;
                info.required_space = storageInfo.requiredSpace;
                info.force_required_space = storageInfo.forceRequiredSpace;
                info.free_space = storageInfo.freeSpace;
                info.storage = options_.storage;
                for (const auto& prepared : task_->GetPreparedContents()) {
                    user_backend::PackageContentInfo content{};
                    if (!describe_content(prepared, content, error)) return false;
                    info.contents.push_back(content);
                }
                if (!info.contents.empty())
                    load_installed_title_metadata(info.contents.front().application_id, info);
                const auto decision = options_.preflight(info);
                if (decision == user_backend::PreflightDecision::Skip) {
                    skipped_ = true;
                    return false;
                }
                if (decision == user_backend::PreflightDecision::Cancel) {
                    cancel.store(true);
                    error = "installation cancelled before content write";
                    return false;
                }
                if (decision == user_backend::PreflightDecision::ForceInstall) {
                    if (info.force_required_space > storageInfo.freeSpace) {
                        error = "not enough temporary destination space for force reinstall: need " +
                                std::to_string(info.force_required_space) + " bytes, free " +
                                std::to_string(storageInfo.freeSpace);
                        return false;
                    }
                    task_->SetForceReinstall(true);
                }
            }
            if (progress) progress({input.size() / 2, input.size(), "AtmoXL Begin"});
            if (cancel.load()) { error = "installation cancelled"; return false; }
            task_->Begin();
            if (progress) progress({input.size(), input.size(), "AtmoXL Complete"});
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
#else
        (void)input; (void)progress; (void)cancel;
        error = "AtmoXL installer requires a Switch/libnx build";
        return false;
#endif
    }

    bool finalize(std::string&) override { return true; }

    bool verify(std::string& error) override {
#ifdef __SWITCH__
        if (!task_) { error = "installer task is unavailable for verification"; return false; }
        try {
            task_->Verify();
            error = task_->GetWarning();
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
#else
        (void)error;
#endif
        return true;
    }

    bool skipped() const override { return skipped_; }

    void close() override {
#ifdef __SWITCH__
        astranas::atmo_xl_bridge::clear_install_hooks();
        progress_ = nullptr;
        cancel_ = nullptr;
        task_.reset();
        if (services_initialized_) {
            inst::util::deinitInstallServices();
            services_initialized_ = false;
        }
        if (exit_locked_) {
            appletUnlockExit();
            exit_locked_ = false;
        }
#endif
    }

private:
    static bool forward_progress(const char* stage, std::uint64_t current,
                                 std::uint64_t total, void* context) {
        auto* self = static_cast<AtmoXLProvider*>(context);
        if (!self || (self->cancel_ && self->cancel_->load())) return false;
        if (self->progress_ && *self->progress_)
            (*self->progress_)({current, total, std::string("Installing ") + (stage ? stage : "content")});
        return !self->cancel_ || !self->cancel_->load();
    }

    static bool forward_invalid_nca(const char* content_id, void* context) {
        auto* self = static_cast<AtmoXLProvider*>(context);
        if (!self || !self->options_.invalid_nca) return false;
        const bool accepted = self->options_.invalid_nca(content_id ? content_id : "");
        if (accepted) {
            self->options_.validate_nca = false;
#ifdef __SWITCH__
            inst::config::validateNCAs = false;
#endif
        }
        return accepted;
    }

    PackageContainerKind kind_ = PackageContainerKind::None;
    user_backend::InstallOptions options_{};
    PackageInspection inspection_{};
    const user_backend::ProgressCallback* progress_ = nullptr;
    std::atomic_bool* cancel_ = nullptr;
    bool skipped_ = false;
#ifdef __SWITCH__
    std::unique_ptr<tin::install::Install> task_;
    bool services_initialized_ = false;
    bool exit_locked_ = false;
#endif
};
}

std::unique_ptr<BackendProvider> create_backend_provider(
    PackageContainerKind kind, const user_backend::InstallOptions& options) {
    return std::make_unique<AtmoXLProvider>(kind, options);
}
} // namespace astranas::title_backend
