// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "package_inspect.hpp"
#include "user_title_backend.hpp"
#include "package_source.hpp"

#include <atomic>
#include <memory>

namespace astranas::title_backend {
class BackendProvider {
public:
    virtual ~BackendProvider() = default;
    virtual bool supports(PackageContainerKind kind) const = 0;
    virtual bool begin_session(const PackageInspection&, std::string& error) = 0;
    virtual bool write_content(PackageSource&, const user_backend::ProgressCallback&, std::atomic_bool&, std::string& error) = 0;
    virtual bool finalize(std::string& error) = 0;
    virtual bool verify(std::string& error) = 0;
    virtual bool skipped() const { return false; }
    virtual void close() = 0;
};
std::unique_ptr<BackendProvider> create_backend_provider(
    PackageContainerKind kind, const user_backend::InstallOptions& options = {});
} // namespace astranas::title_backend
