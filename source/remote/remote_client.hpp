// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../config.hpp"
#include "../model.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using TransferProgressCallback = std::function<bool(std::uint64_t transferred, std::uint64_t total)>;
using DownloadProgressCallback = TransferProgressCallback;
using UploadProgressCallback = TransferProgressCallback;
using RemoteStreamDataCallback = std::function<bool(const unsigned char* data,
                                                    std::size_t size,
                                                    std::uint64_t logical_offset)>;

struct RemoteBenchmarkStats {
    double mib_per_sec = 0.0;
    double seconds = 0.0;
    std::uint64_t bytes = 0;
    std::uint64_t request_count = 0;
    std::uint64_t average_request_bytes = 0;
    std::uint32_t negotiated_read_size = 0;
    std::uint32_t pipeline_request_size = 0;
    std::uint32_t pipeline_window = 0;
    std::uint32_t requested_socket_recv_buffer = 0;
    std::uint32_t socket_recv_buffer = 0;
    std::uint64_t receive_callback_count = 0;
    std::uint64_t average_callback_bytes = 0;
};

class RemoteClient {
public:
    virtual ~RemoteClient() = default;
    virtual bool connect(const AppConfig& config, std::string& error) = 0;
    virtual bool fetch_text(const std::string& remote_path, std::string& text, std::string& error) = 0;
    virtual bool list_dir(const std::string& remote_path, std::vector<RemoteDirEntry>& entries, std::string& error) = 0;
    virtual bool download(const RemoteDirEntry& entry, const std::string& local_path,
                          std::uint64_t& transferred, std::uint64_t& total,
                          const DownloadProgressCallback& progress, std::string& error) = 0;
    virtual bool download_responsive(const RemoteDirEntry& entry, const std::string& local_path,
                                     std::uint64_t& transferred, std::uint64_t& total,
                                     const DownloadProgressCallback& progress, std::string& error) {
        return download(entry, local_path, transferred, total, progress, error);
    }
    virtual bool upload(const std::string& local_path, const std::string& remote_path,
                        std::uint64_t& transferred, std::uint64_t& total,
                        const UploadProgressCallback& progress, std::string& error) = 0;
    virtual bool supports_delete() const { return false; }
    virtual bool delete_entry(const RemoteDirEntry& entry, bool recursive, std::string& error) {
        (void)entry;
        (void)recursive;
        error = "当前协议不支持远程删除";
        return false;
    }
    virtual void set_benchmark_socket_recv_buffer(std::uint32_t bytes) { (void)bytes; }
    virtual bool benchmark(const std::string& remote_path, std::size_t bytes,
                           double& mib_per_sec, std::string& error) = 0;
    virtual bool benchmark_detailed(const std::string& remote_path, std::size_t bytes,
                                    RemoteBenchmarkStats& stats, std::string& error) {
        double speed = 0.0;
        if (!benchmark(remote_path, bytes, speed, error)) return false;
        stats = {};
        stats.mib_per_sec = speed;
        return true;
    }

    virtual bool prepare_direct_read(const RemoteDirEntry& entry, std::string& error) = 0;
    virtual bool read_range(const RemoteDirEntry& entry, std::uint64_t offset,
                            void* buffer, std::size_t size, std::size_t& actual,
                            std::string& error) = 0;
    virtual bool stream_range(const RemoteDirEntry& entry, std::uint64_t offset,
                              std::uint64_t size, const RemoteStreamDataCallback& data,
                              const TransferProgressCallback& progress,
                              std::string& error) {
        if (!data) { error = "连续流消费者不可用"; return false; }
        constexpr std::size_t kFallbackChunk = 1024u * 1024u;
        std::vector<unsigned char> buffer(kFallbackChunk);
        std::uint64_t done = 0;
        while (done < size) {
            if (progress && !progress(done, size)) { error = "cancelled"; return false; }
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), size - done));
            std::size_t actual = 0;
            if (!read_range(entry, offset + done, buffer.data(), want, actual, error)) return false;
            if (actual != want) { error = "连续流源提前结束"; return false; }
            if (!data(buffer.data(), actual, done)) { error = "installation cancelled"; return false; }
            done += actual;
        }
        if (progress && !progress(done, size)) { error = "cancelled"; return false; }
        return true;
    }
    virtual bool finish_direct_read(const RemoteDirEntry& entry, std::string& error) = 0;
    virtual void close_direct_read() = 0;
    virtual std::string protocol_name() const = 0;
};

std::unique_ptr<RemoteClient> make_remote_client(const AppConfig& config, std::string& error);
std::string join_remote_path(const std::string& root, const std::string& child);
