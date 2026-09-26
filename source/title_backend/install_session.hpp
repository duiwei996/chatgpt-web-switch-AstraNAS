// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "backend_provider.hpp"
namespace astranas::title_backend {
class InstallSession {
public:
    explicit InstallSession(std::unique_ptr<BackendProvider> provider) : provider_(std::move(provider)) {}
    user_backend::Result run(const std::string& path, const user_backend::ProgressCallback&, std::atomic_bool&, std::string& error);
    user_backend::Result run(PackageSource& source, const PackageInspection& info,
                             const user_backend::ProgressCallback&, std::atomic_bool&,
                             std::string& error);
private:
    std::unique_ptr<BackendProvider> provider_;
};
}
