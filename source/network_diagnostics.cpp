// SPDX-License-Identifier: GPL-3.0-or-later
#include "network_diagnostics.hpp"
#include <iomanip>
#include <sstream>
#include <vector>

namespace astranas::network {

bool run_remote_diagnostic(RemoteClient& remote,
                           const std::string& start_dir,
                           std::size_t benchmark_bytes,
                           DiagnosticResult& result,
                           std::string& error,
                           std::size_t max_scan_dirs) {
    result = {};
    error.clear();
    std::vector<std::string> pending{start_dir.empty() ? "/" : start_dir};
    std::size_t cursor = 0;
    std::string last_list_error;

    while (cursor < pending.size() && cursor < max_scan_dirs) {
        const std::string dir = pending[cursor++];
        std::vector<RemoteDirEntry> entries;
        std::string list_error;
        if (!remote.list_dir(dir, entries, list_error)) {
            if (!list_error.empty()) last_list_error = list_error;
            continue;
        }
        for (const auto& entry : entries) {
            if (!entry.is_dir && entry.size > result.candidate.size)
                result.candidate = entry;
        }
        if (result.candidate.size >= benchmark_bytes) break;
        for (const auto& entry : entries) {
            if (entry.is_dir && pending.size() < max_scan_dirs)
                pending.push_back(entry.path);
        }
    }

    if (result.candidate.path.empty() || result.candidate.size == 0) {
        error = last_list_error.empty()
            ? "当前目录附近没有可用于测速的普通文件"
            : "无法找到测速文件：" + last_list_error;
        return false;
    }

    const std::size_t bytes = static_cast<std::size_t>(
        std::min<std::uint64_t>(result.candidate.size, benchmark_bytes));
    if (!remote.benchmark_detailed(result.candidate.path, bytes, result.stats, error))
        return false;
    return true;
}

std::string format_diagnostic_summary(const std::string& protocol,
                                      const DiagnosticResult& result,
                                      const std::string& socket_profile) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out << std::setprecision(2);
    if (!socket_profile.empty()) out << "Socket：" << socket_profile << '\n';
    out << "协议：" << protocol
        << "\n文件：" << result.candidate.name
        << "\n纯 RAM 吞吐：" << result.stats.mib_per_sec << " MiB/s"
        << "\n测试数据：" << (result.stats.bytes / 1024.0 / 1024.0) << " MiB"
        << "\n耗时：" << result.stats.seconds << " s"
        << "\n请求次数：" << result.stats.request_count
        << "\n平均请求：" << (result.stats.average_request_bytes / 1024.0) << " KiB";
    if (result.stats.negotiated_read_size)
        out << "\nSMB 协商读上限：" << (result.stats.negotiated_read_size / 1024.0) << " KiB";
    if (result.stats.pipeline_request_size)
        out << "\n流水线请求：" << (result.stats.pipeline_request_size / 1024.0) << " KiB";
    if (result.stats.pipeline_window)
        out << "\n并发窗口：" << result.stats.pipeline_window;
    if (result.stats.socket_recv_buffer)
        out << "\n实际 SO_RCVBUF：" << (result.stats.socket_recv_buffer / 1024.0) << " KiB";
    if (result.stats.receive_callback_count)
        out << "\n接收回调：" << result.stats.receive_callback_count
            << " 次，平均 " << (result.stats.average_callback_bytes / 1024.0) << " KiB";
    return out.str();
}

std::string diagnostic_hint(const std::string& protocol, const DiagnosticResult& result) {
    const double speed = result.stats.mib_per_sec;
    if (speed <= 0.0) return {};
    if (speed >= 9.5 && speed <= 12.8)
        return "吞吐停在百兆级：若同一 NAS 的手机/电脑明显更快，重点排查 Switch WLAN/BSD 网络栈";
    if (protocol == "smb" && result.stats.pipeline_window >= 8 && speed < 8.0)
        return "SMB 已启用 8 路预取但吞吐仍低：继续检查 Wi-Fi、NAS 磁盘和 SMB 服务端";
    if (protocol == "webdav" && result.stats.request_count == 1 && speed < 8.0)
        return "HTTP 单 Range 连续流仍偏低：瓶颈更可能在 Wi-Fi/TCP/NAS 链路，而不是请求次数";
    if (speed >= 20.0) return "网络纯 RAM 吞吐充足；若直装明显更慢，重点看 NCM/SD 写入与解压处理";
    return "把该结果与安装完成后的“源 / NCM / 等网 / 回压”指标对照，可定位直装瓶颈";
}

} // namespace astranas::network
