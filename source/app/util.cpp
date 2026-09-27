// SPDX-License-Identifier: GPL-3.0-or-later
#include "util.hpp"
#ifdef __SWITCH__
#include "constants.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>

namespace astranas::app {
std::string format_size(std::uint64_t bytes) {
    char buf[64]{};
    const double kb = static_cast<double>(bytes) / 1024.0;
    const double mb = kb / 1024.0;
    const double gb = mb / 1024.0;
    if (gb >= 1.0) std::snprintf(buf, sizeof(buf), "%.2f GB", gb);
    else if (mb >= 1.0) std::snprintf(buf, sizeof(buf), "%.1f MB", mb);
    else if (kb >= 1.0) std::snprintf(buf, sizeof(buf), "%.1f KB", kb);
    else std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    return buf;
}
std::string format_speed(double mib_per_sec) {
    char buf[64]{}; std::snprintf(buf, sizeof(buf), "%.2f MB/s", mib_per_sec); return buf;
}
std::string basename_of(std::string path) {
    while (!path.empty() && path.back() == '/') path.pop_back();
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}
std::uint64_t local_file_size(const std::string& path) {
    struct stat st{}; if (stat(path.c_str(), &st) != 0 || st.st_size < 0) return 0;
    return static_cast<std::uint64_t>(st.st_size);
}
std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
void append_debug_log(const std::string& context, const std::string& detail) {
    if (detail.empty()) return;
    std::FILE* fp = std::fopen(kLogPath, "ab"); if (!fp) return;
    std::fprintf(fp, "%s: %s\n", context.c_str(), detail.c_str()); std::fclose(fp);
}
std::string friendly_error(const std::string& action, const std::string& raw) {
    append_debug_log(action, raw);
    const std::string lower = lower_copy(raw);
    std::string reason = "发生错误，请检查设置后重试";
    const bool http_context = lower.find("http") != std::string::npos ||
                              lower.find("webdav") != std::string::npos ||
                              lower.find("curl") != std::string::npos ||
                              lower.find("response") != std::string::npos;
    if (lower.find("hierarchical sha-256") != std::string::npos ||
        lower.find("0x001fd602") != std::string::npos ||
        lower.find("2002-4075") != std::string::npos) {
        if (lower.find("source_vs_registered_body_sha256=different") != std::string::npos)
            reason = "系统拒绝挂载 CNMT NCA，且源与注册后主体 SHA-256 不同；请查看日志中的审计值，并尝试下载到本机后安装";
        else if (lower.find("source_vs_registered_body_sha256=match") != std::string::npos)
            reason = "系统拒绝挂载 CNMT NCA，但源与注册后主体 SHA-256 相同；请查看日志中的头部审计和系统兼容信息";
        else
            reason = "系统拒绝挂载 CNMT NCA；源/注册后主体 SHA-256 对照已写入日志";
    }
    else if (lower.find("安装预读缓冲") != std::string::npos)
        reason = "安装预读缓冲内存不足，请关闭其他程序后重试";
    else if (lower.find("cancel") != std::string::npos) reason = "操作已取消";
    else if (lower.find("timeout") != std::string::npos || lower.find("timed out") != std::string::npos) reason = "连接超时，请检查网络和服务器状态";
    else if ((http_context && (lower.find("401") != std::string::npos || lower.find("403") != std::string::npos)) ||
             lower.find("auth") != std::string::npos || lower.find("password") != std::string::npos ||
             lower.find("credential") != std::string::npos || lower.find("logon failure") != std::string::npos)
        reason = "身份验证失败，请检查用户名和密码";
    else if (lower.find("404") != std::string::npos || lower.find("not found") != std::string::npos || lower.find("no such") != std::string::npos) reason = "目标不存在，请刷新目录或检查路径";
    else if (lower.find("certificate") != std::string::npos || lower.find("tls") != std::string::npos || lower.find("ssl") != std::string::npos) reason = "安全连接验证失败，请检查证书或 TLS 设置";
    else if (lower.find("resolve") != std::string::npos || lower.find("dns") != std::string::npos || lower.find("host") != std::string::npos) reason = "无法解析服务器地址，请检查地址和 DNS";
    else if (lower.find("connect") != std::string::npos || lower.find("connection") != std::string::npos || lower.find("network") != std::string::npos) reason = "无法连接服务器，请检查网络和 NAS 状态";
    else if (lower.find("space") != std::string::npos || lower.find("disk full") != std::string::npos || lower.find("no space") != std::string::npos) reason = "存储空间不足";
    else if (lower.find("permission") != std::string::npos || lower.find("denied") != std::string::npos || lower.find("access") != std::string::npos) reason = "权限不足，请检查账号或存储权限";
    else if (lower.find("unsupported") != std::string::npos || lower.find("not supported") != std::string::npos) reason = "当前格式或功能不受支持";
    else if (lower.find("invalid") != std::string::npos || lower.find("malformed") != std::string::npos || lower.find("corrupt") != std::string::npos) reason = "数据格式无效或文件已损坏";
    else if (lower.find("changed") != std::string::npos || lower.find("etag") != std::string::npos) reason = "远端文件已发生变化，请刷新目录后重试";
    return action + "：" + reason;
}
std::string trim_remote(std::string path) {
    if (path.empty()) return "/"; while (path.size() > 1 && path.back() == '/') path.pop_back(); return path;
}
bool remote_has_unsafe_component(const std::string& path) {
    if (std::any_of(path.begin(), path.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f || c == '\\'; })) return true;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start); const auto length = (end == std::string::npos ? path.size() : end) - start;
        if ((length == 1 && path.compare(start, 1, ".") == 0) || (length == 2 && path.compare(start, 2, "..") == 0)) return true;
        if (end == std::string::npos) break; start = end + 1;
    }
    return false;
}
bool remote_path_within(const std::string& root, const std::string& path) {
    const std::string r = trim_remote(root), p = trim_remote(path);
    if (remote_has_unsafe_component(r) || remote_has_unsafe_component(p)) return false;
    if (r == "/") return !p.empty() && p.front() == '/';
    if (p == r) return true;
    return p.size() > r.size() && p.compare(0, r.size(), r) == 0 && p[r.size()] == '/';
}
std::string remote_parent_within(const std::string& root, const std::string& current) {
    const std::string r = trim_remote(root); std::string c = trim_remote(current);
    if (!remote_path_within(r, c) || c == r) return r;
    const auto slash = c.find_last_of('/'); if (slash == std::string::npos) return r;
    if (slash == 0) c = "/"; else c.resize(slash);
    return remote_path_within(r, c) ? c : r;
}
std::string local_parent_within(std::string root, std::string current) {
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    while (current.size() > 1 && current.back() == '/') current.pop_back();
    if (current == root) return root + "/";
    if (current.size() <= root.size() || current.compare(0, root.size(), root) != 0 || current[root.size()] != '/') return root + "/";
    const auto slash = current.find_last_of('/'); if (slash == std::string::npos || slash <= root.size()) return root + "/";
    return current.substr(0, slash);
}
std::string title_id_text(std::uint64_t id) {
    std::ostringstream out; out << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << id; return out.str();
}
} // namespace astranas::app
#endif
