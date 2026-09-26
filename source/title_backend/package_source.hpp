// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace astranas::title_backend {

using PackageStreamDataCallback = std::function<bool(const unsigned char* data,
                                                     std::size_t size,
                                                     std::uint64_t logical_offset)>;
using PackageStreamProgressCallback = std::function<bool(std::uint64_t transferred,
                                                         std::uint64_t total)>;

class PackageSource {
public:
    virtual ~PackageSource() = default;
    virtual bool open(const std::string& path, std::string& error) = 0;
    virtual bool read(void* buffer, std::size_t size, std::size_t& actual, std::string& error) = 0;
    virtual bool seek(std::uint64_t offset, std::string& error) = 0;
    virtual std::uint64_t size() const = 0;
    // Optional cancellation bridge for blocking/remote PackageSource implementations.
    // Local sources intentionally keep the default no-op implementation.
    virtual void set_cancel_flag(std::atomic_bool*) {}
    // Remote sources may expose a native contiguous stream so bulk NCA/NCZ payloads
    // stay on one transport transaction instead of reopening an 8 MiB range for
    // every read-ahead slot. Local/split sources keep the normal read_at path.
    virtual bool supports_contiguous_stream() const { return false; }
    virtual bool stream_contiguous(std::uint64_t offset, std::uint64_t total_size,
                                   const PackageStreamDataCallback& data,
                                   const PackageStreamProgressCallback& progress,
                                   std::string& error) {
        (void)offset; (void)total_size; (void)data; (void)progress;
        error = "安装包源不支持连续流读取";
        return false;
    }
    bool read_at(std::uint64_t offset, void* buffer, std::size_t size,
                 std::size_t& actual, std::string& error);
};

class FilePackageSource final : public PackageSource {
public:
    ~FilePackageSource() override;
    bool open(const std::string& path, std::string& error) override;
    bool read(void* buffer, std::size_t size, std::size_t& actual, std::string& error) override;
    bool seek(std::uint64_t offset, std::string& error) override;
    std::uint64_t size() const override { return size_; }
    const std::string& path() const { return path_; }
    const std::vector<std::string>& part_paths() const { return part_paths_; }
    bool split() const { return part_paths_.size() > 1; }

private:
    struct Part {
        void* file = nullptr;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
    };
    void close();
    std::vector<Part> parts_;
    std::vector<std::string> part_paths_;
    std::uint64_t size_ = 0;
    std::uint64_t position_ = 0;
    std::string path_;
};

// Resolves a regular package, a libnx concatenation-file path, a directory
// containing numeric parts (00, 01, ...), or common .ns0/.xc0 and
// .nsp.00/.xci.00 part naming. Only the first part is an install candidate.
bool resolve_package_parts(const std::string& path, std::vector<std::string>& parts,
                           std::string& error);
struct SplitPackagePartInfo {
    std::string prefix;
    std::size_t width = 0;
    std::uint64_t index = 0;
};
bool describe_split_package_part(const std::string& path, SplitPackagePartInfo& info);
bool is_split_package_first_part(const std::string& path);
bool is_split_package_later_part(const std::string& path);

} // namespace astranas::title_backend
