// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "remote_client.hpp"

class CurlRemoteClient : public RemoteClient {
public:
    ~CurlRemoteClient() override;
    bool connect(const AppConfig& config, std::string& error) override;
    bool fetch_text(const std::string& remote_path, std::string& text, std::string& error) override;
    bool list_dir(const std::string& remote_path, std::vector<RemoteDirEntry>& entries, std::string& error) override;
    bool download(const RemoteDirEntry& entry, const std::string& local_path,
                  std::uint64_t& transferred, std::uint64_t& total,
                  const DownloadProgressCallback& progress, std::string& error) override;
    bool download_responsive(const RemoteDirEntry& entry, const std::string& local_path,
                             std::uint64_t& transferred, std::uint64_t& total,
                             const DownloadProgressCallback& progress, std::string& error) override;
    bool upload(const std::string& local_path, const std::string& remote_path,
                std::uint64_t& transferred, std::uint64_t& total,
                const UploadProgressCallback& progress, std::string& error) override;
    bool supports_delete() const override { return is_webdav_; }
    bool delete_entry(const RemoteDirEntry& entry, bool recursive, std::string& error) override;
    void set_benchmark_socket_recv_buffer(std::uint32_t bytes) override { benchmark_recv_buffer_ = bytes; }
    bool benchmark(const std::string& remote_path, std::size_t bytes,
                   double& mib_per_sec, std::string& error) override;
    bool benchmark_detailed(const std::string& remote_path, std::size_t bytes,
                            RemoteBenchmarkStats& stats, std::string& error) override;
    bool prepare_direct_read(const RemoteDirEntry& entry, std::string& error) override;
    bool read_range(const RemoteDirEntry& entry, std::uint64_t offset, void* buffer,
                    std::size_t size, std::size_t& actual, std::string& error) override;
    bool stream_range(const RemoteDirEntry& entry, std::uint64_t offset,
                      std::uint64_t size, const RemoteStreamDataCallback& data,
                      const TransferProgressCallback& progress, std::string& error) override;
    bool finish_direct_read(const RemoteDirEntry& entry, std::string& error) override;
    void close_direct_read() override;
    std::string protocol_name() const override { return protocol_name_; }
protected:
    AppConfig config_;
    std::string base_url_;
    std::string protocol_name_ = "CURL";
    bool is_webdav_ = false;
    bool is_ftp_ = false;
    std::uint32_t benchmark_recv_buffer_ = 0;
    void* direct_curl_ = nullptr;
    RemoteDirEntry direct_entry_;
    std::string make_url(const std::string& remote_path) const;
    void apply_common(void* curl_handle) const;
    bool reset_direct_handle(std::string& error);
};
