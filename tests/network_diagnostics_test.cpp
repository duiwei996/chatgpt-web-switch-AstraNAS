// SPDX-License-Identifier: GPL-3.0-or-later
#include "network_diagnostics.hpp"

#include <cassert>
#include <string>
#include <vector>

namespace {
class DiagnosticRemote final : public RemoteClient {
public:
    bool connect(const AppConfig&, std::string&) override { return true; }
    bool fetch_text(const std::string&, std::string&, std::string&) override { return false; }
    bool list_dir(const std::string& path, std::vector<RemoteDirEntry>& entries,
                  std::string&) override {
        entries.clear();
        if (path == "/") {
            entries.push_back({"small.bin", "/small.bin", "small", 8ull * 1024ull * 1024ull, false});
            entries.push_back({"games", "/games", "dir", 0, true});
            return true;
        }
        if (path == "/games") {
            entries.push_back({"large.nsp", "/games/large.nsp", "large", 96ull * 1024ull * 1024ull, false});
            return true;
        }
        return false;
    }
    bool download(const RemoteDirEntry&, const std::string&, std::uint64_t&, std::uint64_t&,
                  const DownloadProgressCallback&, std::string&) override { return false; }
    bool upload(const std::string&, const std::string&, std::uint64_t&, std::uint64_t&,
                const UploadProgressCallback&, std::string&) override { return false; }
    bool benchmark(const std::string& path, std::size_t bytes, double& speed,
                   std::string& error) override {
        RemoteBenchmarkStats stats;
        if (!benchmark_detailed(path, bytes, stats, error)) return false;
        speed = stats.mib_per_sec;
        return true;
    }
    bool benchmark_detailed(const std::string& path, std::size_t bytes,
                            RemoteBenchmarkStats& stats, std::string&) override {
        benchmark_path = path;
        benchmark_bytes = bytes;
        stats = {};
        stats.bytes = bytes;
        stats.seconds = 64.0 / 11.5;
        stats.mib_per_sec = 11.5;
        stats.request_count = 64;
        stats.average_request_bytes = 1024u * 1024u;
        stats.pipeline_request_size = 1024u * 1024u;
        stats.pipeline_window = 8;
        stats.socket_recv_buffer = 4u * 1024u * 1024u;
        stats.receive_callback_count = 128;
        stats.average_callback_bytes = bytes / 128;
        return true;
    }
    bool prepare_direct_read(const RemoteDirEntry&, std::string&) override { return false; }
    bool read_range(const RemoteDirEntry&, std::uint64_t, void*, std::size_t,
                    std::size_t&, std::string&) override { return false; }
    bool finish_direct_read(const RemoteDirEntry&, std::string&) override { return false; }
    void close_direct_read() override {}
    std::string protocol_name() const override { return "SMB"; }

    std::string benchmark_path;
    std::size_t benchmark_bytes = 0;
};
}

int main() {
    DiagnosticRemote remote;
    astranas::network::DiagnosticResult result;
    std::string error;
    constexpr std::size_t kBenchmark = 64u * 1024u * 1024u;
    assert(astranas::network::run_remote_diagnostic(remote, "/", kBenchmark, result, error));
    assert(result.candidate.path == "/games/large.nsp");
    assert(remote.benchmark_path == result.candidate.path);
    assert(remote.benchmark_bytes == kBenchmark);
    const std::string hint = astranas::network::diagnostic_hint("smb", result);
    assert(hint.find("Switch WLAN/BSD") != std::string::npos);
    const std::string summary = astranas::network::format_diagnostic_summary("SMB", result);
    assert(summary.find("SO_RCVBUF") != std::string::npos);
    assert(summary.find("接收回调") != std::string::npos);

    result.stats.mib_per_sec = 24.0;
    assert(astranas::network::diagnostic_hint("webdav", result).find("NCM") != std::string::npos);
    return 0;
}
