// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "remote_client.hpp"
#include "../title_backend/package_source.hpp"
#include <atomic>
#include <utility>

namespace astranas::remote {

class RemotePackageSource final : public title_backend::PackageSource {
public:
    RemotePackageSource(RemoteClient& remote, RemoteDirEntry entry)
        : remote_(remote), entry_(std::move(entry)) {}
    ~RemotePackageSource() override;

    bool open(const std::string& path, std::string& error) override;
    bool read(void* buffer, std::size_t size, std::size_t& actual, std::string& error) override;
    bool seek(std::uint64_t offset, std::string& error) override;
    std::uint64_t size() const override { return entry_.size; }
    void set_cancel_flag(std::atomic_bool* cancel) override { cancel_ = cancel; }
    bool supports_contiguous_stream() const override { return true; }
    bool stream_contiguous(std::uint64_t offset, std::uint64_t total_size,
                           const title_backend::PackageStreamDataCallback& data,
                           const title_backend::PackageStreamProgressCallback& progress,
                           std::string& error) override;

    bool finish(std::string& error);
    bool prepared() const { return prepared_; }
    const RemoteDirEntry& entry() const { return entry_; }

private:
    RemoteClient& remote_;
    RemoteDirEntry entry_;
    std::uint64_t position_ = 0;
    std::atomic_bool* cancel_ = nullptr;
    bool prepared_ = false;
    bool finished_ = false;
};

} // namespace astranas::remote
