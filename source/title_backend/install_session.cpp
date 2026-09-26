// SPDX-License-Identifier: GPL-3.0-or-later
#include "install_session.hpp"
#include "package_inspect.hpp"
#include <exception>

namespace {
class ProviderCloseGuard {
public:
    explicit ProviderCloseGuard(astranas::title_backend::BackendProvider* provider)
        : provider_(provider) {}
    ~ProviderCloseGuard() {
        try { if (provider_) provider_->close(); }
        catch (...) {}
    }
private:
    astranas::title_backend::BackendProvider* provider_;
};
}

namespace astranas::title_backend {
user_backend::Result InstallSession::run(const std::string& path, const user_backend::ProgressCallback& progress, std::atomic_bool& cancel, std::string& error) {
    error.clear();
    if (cancel.load()) { error = "安装已取消"; return user_backend::Result::Cancelled; }
    PackageInspection info;
    if (!inspect_package_file(path, info, error)) return user_backend::Result::Failed;
    if (!provider_ || !provider_->supports(info.kind)) { if (error.empty()) error = "没有可用的安装 provider: " + std::string(package_container_label(info.kind)); return user_backend::Result::Unsupported; }
    FilePackageSource source;
    if (!source.open(path, error)) return user_backend::Result::Failed;
    return run(source, info, progress, cancel, error);
}

user_backend::Result InstallSession::run(PackageSource& source, const PackageInspection& info,
                                         const user_backend::ProgressCallback& progress,
                                         std::atomic_bool& cancel, std::string& error) {
    error.clear();
    if (cancel.load()) { error = "安装已取消"; return user_backend::Result::Cancelled; }
    if (!provider_ || !provider_->supports(info.kind)) {
        error = "没有可用的安装 provider: " + std::string(package_container_label(info.kind));
        return user_backend::Result::Unsupported;
    }
    ProviderCloseGuard closeGuard(provider_.get());
    try {
        if (progress) progress({0, source.size(), "Preparing " + std::string(package_container_label(info.kind))});
        if (!provider_->begin_session(info, error)) return user_backend::Result::Failed;
        if (!provider_->write_content(source, progress, cancel, error)) {
            if (provider_->skipped()) return user_backend::Result::Skipped;
            return cancel.load() ? user_backend::Result::Cancelled : user_backend::Result::Failed;
        }
        if (!provider_->finalize(error)) return user_backend::Result::Failed;
        if (progress) progress({source.size(), source.size(), "Finalizing"});
        if (!provider_->verify(error)) return user_backend::Result::Failed;
        if (progress) progress({source.size(), source.size(), "Verified"});
        return user_backend::Result::Success;
    } catch (const std::exception& exception) {
        error = exception.what();
        return cancel.load() ? user_backend::Result::Cancelled : user_backend::Result::Failed;
    } catch (...) {
        error = "installer provider raised an unknown exception";
        return cancel.load() ? user_backend::Result::Cancelled : user_backend::Result::Failed;
    }
}
} // namespace astranas::title_backend
