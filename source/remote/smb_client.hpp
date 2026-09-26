// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "remote_client.hpp"
struct smb2_context;
struct smb2fh;
class SmbRemoteClient final : public RemoteClient {
public:
    ~SmbRemoteClient() override;
    bool connect(const AppConfig& config,std::string& error) override;
    bool fetch_text(const std::string& remote_path,std::string& text,std::string& error) override;
    bool list_dir(const std::string& remote_path,std::vector<RemoteDirEntry>& entries,std::string& error) override;
    bool download(const RemoteDirEntry& entry,const std::string& local_path,std::uint64_t& transferred,std::uint64_t& total,const DownloadProgressCallback& progress,std::string& error) override;
    bool upload(const std::string& local_path,const std::string& remote_path,std::uint64_t& transferred,std::uint64_t& total,const UploadProgressCallback& progress,std::string& error) override;
    bool supports_delete() const override { return true; }
    bool delete_entry(const RemoteDirEntry& entry,bool recursive,std::string& error) override;
    void set_benchmark_socket_recv_buffer(std::uint32_t bytes) override;
    bool benchmark(const std::string& remote_path,std::size_t bytes,double& mib_per_sec,std::string& error) override {
        RemoteBenchmarkStats stats;
        if(!benchmark_detailed(remote_path,bytes,stats,error)) return false;
        mib_per_sec=stats.mib_per_sec;
        return true;
    }
    bool benchmark_detailed(const std::string& remote_path,std::size_t bytes,RemoteBenchmarkStats& stats,std::string& error) override;
    bool prepare_direct_read(const RemoteDirEntry& entry,std::string& error) override;
    bool read_range(const RemoteDirEntry& entry,std::uint64_t offset,void* buffer,std::size_t size,std::size_t& actual,std::string& error) override;
    bool stream_range(const RemoteDirEntry& entry,std::uint64_t offset,std::uint64_t size,const RemoteStreamDataCallback& data,const TransferProgressCallback& progress,std::string& error) override;
    bool finish_direct_read(const RemoteDirEntry& entry,std::string& error) override;
    void close_direct_read() override;
    std::string protocol_name() const override{return "SMB2/3";}
private:
    smb2_context* ctx_=nullptr;std::uint32_t max_read_=1024*1024;std::uint32_t benchmark_recv_buffer_=0;std::string server_,username_,password_,domain_,share_hint_,connected_share_;
    smb2fh* direct_file_=nullptr;RemoteDirEntry direct_entry_;std::string direct_share_,direct_path_;int direct_retries_=2;
    void reset();bool rebuild_context(std::string& error);bool ensure_share(const std::string& share,std::string& error);bool resolve_path(const std::string& remote_path,std::string& share,std::string& inner,bool& server_root,std::string& error);bool list_server_shares(std::vector<RemoteDirEntry>& entries,std::string& error);bool remote_info(const std::string& remote_path,RemoteDirEntry& entry,std::string& error);
    bool reopen_direct_file(std::string& error);
};
