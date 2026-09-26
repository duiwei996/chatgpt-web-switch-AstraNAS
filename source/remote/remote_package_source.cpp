// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote_package_source.hpp"

namespace astranas::remote {

RemotePackageSource::~RemotePackageSource() {
    if (prepared_ && !finished_) remote_.close_direct_read();
}

bool RemotePackageSource::open(const std::string& path, std::string& error) {
    if (prepared_) remote_.close_direct_read();
    prepared_ = false;
    finished_ = false;
    position_ = 0;
    if (path != entry_.path) {
        error = "远程安装包路径与所选项目不一致";
        return false;
    }
    if (entry_.is_dir || entry_.size == 0) {
        error = "远程安装包必须是非空文件";
        return false;
    }
    if (!remote_.prepare_direct_read(entry_, error)) return false;
    prepared_ = true;
    return true;
}

bool RemotePackageSource::read(void* buffer, std::size_t size, std::size_t& actual,
                               std::string& error) {
    actual = 0;
    if (!prepared_ || finished_) {
        error = "远程安装包尚未打开";
        return false;
    }
    if (position_ > entry_.size || size > entry_.size - position_) {
        error = "远程安装包读取范围超出文件";
        return false;
    }
    if (cancel_ && cancel_->load()) { error = "cancelled"; return false; }
    if (!remote_.read_range(entry_, position_, buffer, size, actual, error)) return false;
    if (cancel_ && cancel_->load()) { error = "cancelled"; return false; }
    position_ += actual;
    return true;
}

bool RemotePackageSource::seek(std::uint64_t offset, std::string& error) {
    if (!prepared_ || finished_ || offset > entry_.size) {
        error = "远程安装包定位偏移无效";
        return false;
    }
    position_ = offset;
    return true;
}

bool RemotePackageSource::stream_contiguous(std::uint64_t offset, std::uint64_t total_size,
                                            const title_backend::PackageStreamDataCallback& data,
                                            const title_backend::PackageStreamProgressCallback& progress,
                                            std::string& error) {
    if (!prepared_ || finished_) {
        error = "远程安装包尚未打开";
        return false;
    }
    if (offset > entry_.size || total_size > entry_.size - offset) {
        error = "远程安装包连续流范围超出文件";
        return false;
    }
    const TransferProgressCallback cancel_progress = [&](std::uint64_t done, std::uint64_t total) {
        if (cancel_ && cancel_->load()) return false;
        return !progress || progress(done, total);
    };
    const RemoteStreamDataCallback forward = [&](const unsigned char* bytes, std::size_t size,
                                                  std::uint64_t logical_offset) {
        if (cancel_ && cancel_->load()) return false;
        return data && data(bytes, size, logical_offset);
    };
    return remote_.stream_range(entry_, offset, total_size, forward, cancel_progress, error);
}

bool RemotePackageSource::finish(std::string& error) {
    if (!prepared_ || finished_) {
        error = "远程安装包直读会话未激活";
        return false;
    }
    const bool ok = remote_.finish_direct_read(entry_, error);
    finished_ = true;
    prepared_ = false;
    return ok;
}

} // namespace astranas::remote
