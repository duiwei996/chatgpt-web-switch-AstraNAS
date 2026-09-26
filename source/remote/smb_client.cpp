// SPDX-License-Identifier: GPL-3.0-or-later
#include "smb_client.hpp"
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-share-enum.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
constexpr std::uint32_t kSmbPipelineRequestCap = 1024u * 1024u;
constexpr std::size_t kSmbPipelineWindow = 8u;

std::uint32_t socket_recv_buffer(int fd) {
    if (fd < 0) return 0;
    int value = 0;
    socklen_t length = sizeof(value);
    if (::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, &length) != 0 || value <= 0) return 0;
    return static_cast<std::uint32_t>(value);
}

std::string parent_of(std::string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return "/";
    return path.substr(0, slash);
}

std::string basename_of(std::string path) {
    while (!path.empty() && path.back() == '/') path.pop_back();
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool safe_component(const char* name) {
    if (!name || !*name || std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) return false;
    std::size_t count = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p) {
        if (++count > 255 || *p < 0x20 || *p == 0x7f || *p == '/' || *p == '\\') return false;
    }
    return true;
}

std::string trim_slashes(std::string path) {
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    while (!path.empty() && path.back() == '/') path.pop_back();
    return path;
}

std::string smb_identity(const smb2_stat_64& st) {
    return "smb:" + std::to_string(st.smb2_ino) + ":" + std::to_string(st.smb2_size) + ":" +
           std::to_string(st.smb2_mtime) + ":" + std::to_string(st.smb2_mtime_nsec) + ":" +
           std::to_string(st.smb2_ctime) + ":" + std::to_string(st.smb2_ctime_nsec);
}

struct AsyncReadSlot {
    std::vector<std::uint8_t> buffer;
    std::uint64_t offset = 0;
    std::uint32_t requested = 0;
    int status = 0;
    bool in_flight = false;
    bool done = false;
};

void async_read_done(smb2_context*, int status, void*, void* private_data) {
    auto* slot = static_cast<AsyncReadSlot*>(private_data);
    if (!slot) return;
    slot->status = status;
    slot->done = true;
}

// High-throughput ordered SMB reader. Up to eight SMB READ requests are kept in
// flight so Wi-Fi/TCP latency does not leave the receive window idle. All data
// is delivered to the caller in file order even if replies complete out of order.
bool pipeline_read(smb2_context* ctx, smb2fh* file,
                   std::uint64_t start_offset, std::uint64_t total_size,
                   std::uint32_t request_size,
                   const RemoteStreamDataCallback& data,
                   const TransferProgressCallback& progress,
                   std::uint64_t& delivered, std::uint64_t& request_count,
                   std::string& error, std::size_t window = kSmbPipelineWindow) {
    delivered = 0;
    request_count = 0;
    error.clear();
    if (total_size == 0) return true;
    if (!ctx || !file || request_size == 0 || window == 0) {
        error = "SMB 流水线读取参数无效";
        return false;
    }
    window = std::min<std::size_t>(window, 8);
    std::vector<AsyncReadSlot> slots(window);
    for (auto& slot : slots) slot.buffer.resize(request_size);

    std::uint64_t scheduled = 0;
    std::size_t in_flight = 0;
    bool cancel_requested = false;
    bool consumer_failed = false;
    bool failed = false;

    auto schedule_one = [&](AsyncReadSlot& slot) -> bool {
        if (scheduled >= total_size || cancel_requested || consumer_failed || failed) return false;
        const auto want = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(request_size, total_size - scheduled));
        slot.offset = start_offset + scheduled;
        slot.requested = want;
        slot.status = 0;
        slot.done = false;
        slot.in_flight = true;
        const int rc = smb2_pread_async(ctx, file, slot.buffer.data(), want, slot.offset,
                                        async_read_done, &slot);
        if (rc < 0) {
            slot.in_flight = false;
            error = smb2_get_error(ctx) ? smb2_get_error(ctx) : "SMB 异步读取提交失败";
            failed = true;
            return false;
        }
        scheduled += want;
        ++in_flight;
        ++request_count;
        return true;
    };

    for (auto& slot : slots) {
        if (!schedule_one(slot)) break;
    }

    if (progress && !progress(0, total_size)) cancel_requested = true;

    while (in_flight > 0) {
        if (!cancel_requested && progress && !progress(delivered, total_size))
            cancel_requested = true;

        const int fd = smb2_get_fd(ctx);
        if (fd < 0) {
            error = smb2_get_error(ctx) ? smb2_get_error(ctx) : "SMB socket 不可用";
            return false;
        }
        pollfd pfd{fd, static_cast<short>(smb2_which_events(ctx)), 0};
        const int polled = ::poll(&pfd, 1, 25);
        if (polled < 0) {
            error = "SMB 网络等待失败";
            return false;
        }
        if (polled > 0 && pfd.revents != 0) {
            if (smb2_service(ctx, pfd.revents) < 0) {
                error = smb2_get_error(ctx) ? smb2_get_error(ctx) : "SMB 网络服务失败";
                return false;
            }
        }

        // Once cancellation/failure is known, no more data may be delivered. Drain
        // already-issued async requests in any completion order so an error at the
        // current offset cannot strand later completed slots and deadlock the loop.
        if (cancel_requested || consumer_failed || failed) {
            for (auto& slot : slots) {
                if (!slot.in_flight || !slot.done) continue;
                slot.in_flight = false;
                slot.done = false;
                slot.requested = 0;
                --in_flight;
            }
        } else {
            bool made_progress = true;
            while (made_progress) {
                made_progress = false;
                const std::uint64_t wanted_offset = start_offset + delivered;
                for (auto& slot : slots) {
                    if (!slot.in_flight || !slot.done || slot.offset != wanted_offset) continue;
                    made_progress = true;
                    slot.in_flight = false;
                    --in_flight;
                    if (slot.status <= 0) {
                        error = slot.status == 0 ? "SMB 读取在请求范围结束前提前 EOF" :
                            (smb2_get_error(ctx) ? smb2_get_error(ctx) : "SMB 异步读取失败");
                        failed = true;
                    } else if (static_cast<std::uint32_t>(slot.status) != slot.requested) {
                        error = "SMB 服务器返回了非预期的短读取";
                        failed = true;
                    } else if (data &&
                               !data(slot.buffer.data(), static_cast<std::size_t>(slot.status), delivered)) {
                        error = "SMB 数据消费者已停止";
                        consumer_failed = true;
                    }

                    if (!failed && !consumer_failed)
                        delivered += static_cast<std::uint64_t>(std::max(0, slot.status));
                    slot.done = false;
                    slot.requested = 0;
                    if (!cancel_requested && !consumer_failed && !failed) schedule_one(slot);
                    break;
                }
                if (cancel_requested || consumer_failed || failed) break;
            }
        }

        if ((cancel_requested || consumer_failed || failed) && in_flight == 0) break;
        if (!failed && !consumer_failed && !cancel_requested && in_flight == 0 && delivered < total_size) {
            error = "SMB 流水线在完成请求前停止";
            failed = true;
        }
    }

    if (cancel_requested) {
        if (error.empty()) error = "cancelled";
        return false;
    }
    if (consumer_failed || failed) return false;
    if (delivered != total_size) {
        error = "SMB 流水线读取长度不完整";
        return false;
    }
    if (progress && !progress(delivered, total_size)) {
        error = "cancelled";
        return false;
    }
    return true;
}
} // namespace

SmbRemoteClient::~SmbRemoteClient() { close_direct_read(); reset(); }

void SmbRemoteClient::reset() {
    if (ctx_ && direct_file_) { smb2_close(ctx_, direct_file_); direct_file_ = nullptr; }
    if (ctx_) {
        if (!connected_share_.empty()) smb2_disconnect_share(ctx_);
        smb2_destroy_context(ctx_);
        ctx_ = nullptr;
    }
    connected_share_.clear();
}

bool SmbRemoteClient::rebuild_context(std::string& error) {
    reset();
    ctx_ = smb2_init_context();
    if (!ctx_) { error = "smb2_init_context failed"; return false; }
    if (!username_.empty()) smb2_set_user(ctx_, username_.c_str());
    if (!password_.empty()) smb2_set_password(ctx_, password_.c_str());
    if (!domain_.empty()) smb2_set_domain(ctx_, domain_.c_str());
    // Support SMB signing when the server requires it, but do not require it client-side.
    smb2_set_security_mode(ctx_, SMB2_NEGOTIATE_SIGNING_ENABLED);
    smb2_set_timeout(ctx_, 30);
    return true;
}

bool SmbRemoteClient::ensure_share(const std::string& share, std::string& error) {
    if (ctx_ && connected_share_ == share) return true;
    std::string last_error;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!rebuild_context(error)) return false;
        const char* user = username_.empty() ? nullptr : username_.c_str();
        if (smb2_connect_share(ctx_, server_.c_str(), share.c_str(), user) >= 0) {
            connected_share_ = share;
            const auto negotiated = smb2_get_max_read_size(ctx_);
            if (negotiated > 0) max_read_ = std::min<std::uint32_t>(negotiated, 4u * 1024u * 1024u);
            if (max_read_ < 64u * 1024u) max_read_ = 64u * 1024u;
            error.clear();
            return true;
        }
        const std::string detail = smb2_get_error(ctx_) ? smb2_get_error(ctx_) : "SMB connect failed";
        last_error = "SMB share '" + share + "' connect failed: " + detail;
        smb2_destroy_context(ctx_);
        ctx_ = nullptr;
        connected_share_.clear();
        if (attempt < 2) std::this_thread::sleep_for(std::chrono::milliseconds(attempt == 0 ? 200 : 500));
    }
    error = last_error.empty() ? "SMB connection failed after retry" : last_error;
    return false;
}

bool SmbRemoteClient::connect(const AppConfig& config, std::string& error) {
    close_direct_read(); reset(); error.clear();
    if (config.server.empty()) { error = "SMB server address is empty"; return false; }
    server_ = config.server;
    const int port = config.port > 0 ? config.port : 445;
    if (port != 445) server_ += ":" + std::to_string(port);
    username_ = config.username;
    password_ = config.password;
    share_hint_ = trim_slashes(config.share);
    direct_retries_ = std::max(0, config.download_retries);
    domain_.clear();
    const auto slash = username_.find('\\');
    const auto semicolon = username_.find(';');
    const auto sep = slash != std::string::npos ? slash : semicolon;
    if (sep != std::string::npos && sep > 0 && sep + 1 < username_.size()) {
        domain_ = username_.substr(0, sep);
        username_ = username_.substr(sep + 1);
    }
    std::string initial = share_hint_;
    if (initial.empty()) {
        const std::string restored = trim_slashes(config.remote_dir);
        if (!restored.empty()) {
            const auto split = restored.find('/');
            const std::string candidate = split == std::string::npos ? restored : restored.substr(0, split);
            if (safe_component(candidate.c_str())) initial = candidate;
        }
        if (initial.empty()) initial = "IPC$";
    }
    if (!ensure_share(initial, error)) { reset(); return false; }
    return true;
}

bool SmbRemoteClient::resolve_path(const std::string& remote_path, std::string& share,
                                   std::string& inner, bool& server_root, std::string& error) {
    server_root = false; share.clear(); inner.clear();
    if (!ctx_) { error = "SMB not connected"; return false; }
    std::string normalized = trim_slashes(remote_path);
    if (!share_hint_.empty()) { share = share_hint_; inner = normalized; return true; }
    if (normalized.empty()) { server_root = true; share = "IPC$"; return true; }
    const auto slash = normalized.find('/');
    share = slash == std::string::npos ? normalized : normalized.substr(0, slash);
    inner = slash == std::string::npos ? std::string{} : normalized.substr(slash + 1);
    if (!safe_component(share.c_str())) { error = "invalid SMB share name"; return false; }
    return true;
}

bool SmbRemoteClient::list_server_shares(std::vector<RemoteDirEntry>& entries, std::string& error) {
    entries.clear();
    if (!ensure_share("IPC$", error)) return false;
    auto* rep = smb2_share_enum_sync(ctx_, SHARE_INFO_1);
    if (!rep) { error = smb2_get_error(ctx_) ? smb2_get_error(ctx_) : "SMB share enumeration failed"; return false; }
    const auto& level = rep->ses.ShareEnum.Level1;
    for (std::uint32_t i = 0; i < level.EntriesRead; ++i) {
        const auto& info = level.share_info_1[i];
        if (!safe_component(info.netname)) continue;
        if ((info.type & 0x3u) != SRVSVC_SHARE_TYPE_DISKTREE) continue;
        if ((info.type & SRVSVC_SHARE_TYPE_HIDDEN) != 0) continue;
        RemoteDirEntry entry;
        entry.name = info.netname;
        entry.path = "/" + entry.name;
        entry.is_dir = true;
        entry.identity = "smb-share";
        entries.push_back(std::move(entry));
        if (entries.size() >= 256) break;
    }
    smb2_free_data(ctx_, rep);
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return true;
}

bool SmbRemoteClient::fetch_text(const std::string& remote_path, std::string& text, std::string& error) {
    text.clear();
    std::string share, path; bool root = false;
    if (!resolve_path(remote_path, share, path, root, error) || root) {
        if (root) error = "SMB 服务器根目录不是文件";
        return false;
    }
    if (!ensure_share(share, error)) return false;
    smb2fh* file = smb2_open(ctx_, path.c_str(), O_RDONLY);
    if (!file) { error = smb2_get_error(ctx_); return false; }
    std::vector<std::uint8_t> buffer(max_read_);
    std::uint64_t offset = 0;
    constexpr std::uint64_t limit = 8ull * 1024ull * 1024ull;
    for (;;) {
        const int got = smb2_pread(ctx_, file, buffer.data(), static_cast<std::uint32_t>(buffer.size()), offset);
        if (got < 0) { error = smb2_get_error(ctx_); smb2_close(ctx_, file); return false; }
        if (got == 0) break;
        if (offset + static_cast<std::uint64_t>(got) > limit) {
            error = "manifest too large (>8 MiB)"; smb2_close(ctx_, file); return false;
        }
        text.append(reinterpret_cast<const char*>(buffer.data()), static_cast<std::size_t>(got));
        offset += static_cast<std::uint64_t>(got);
    }
    smb2_close(ctx_, file);
    return true;
}

bool SmbRemoteClient::list_dir(const std::string& remote_path, std::vector<RemoteDirEntry>& entries,
                               std::string& error) {
    entries.clear();
    std::string share, path; bool root = false;
    if (!resolve_path(remote_path, share, path, root, error)) return false;
    if (root) return list_server_shares(entries, error);
    if (!ensure_share(share, error)) return false;
    smb2dir* dir = smb2_opendir(ctx_, path.c_str());
    if (!dir) { error = smb2_get_error(ctx_); return false; }
    while (smb2dirent* ent = smb2_readdir(ctx_, dir)) {
        if (!safe_component(ent->name)) continue;
        if (entries.size() >= 16384) { error = "remote directory contains too many entries"; smb2_closedir(ctx_, dir); return false; }
        RemoteDirEntry entry;
        entry.name = ent->name;
        entry.path = remote_path.empty() ? "/" : remote_path;
        if (entry.path.back() != '/') entry.path.push_back('/');
        entry.path += entry.name;
        entry.size = ent->st.smb2_size;
        entry.is_dir = ent->st.smb2_type == SMB2_TYPE_DIRECTORY;
        entry.identity = smb_identity(ent->st);
        entries.push_back(std::move(entry));
    }
    smb2_closedir(ctx_, dir);
    return true;
}

bool SmbRemoteClient::remote_info(const std::string& remote_path, RemoteDirEntry& result, std::string& error) {
    const auto parent = parent_of(remote_path), wanted = basename_of(remote_path);
    std::vector<RemoteDirEntry> entries;
    if (!list_dir(parent, entries, error)) return false;
    for (const auto& entry : entries) {
        if (!entry.is_dir && entry.name == wanted) { result = entry; return true; }
    }
    error = "remote file not found while determining size";
    return false;
}

bool SmbRemoteClient::download(const RemoteDirEntry& entry, const std::string& local_path,
                               std::uint64_t& transferred, std::uint64_t& total,
                               const DownloadProgressCallback& progress, std::string& error) {
    transferred = 0; total = 0;
    RemoteDirEntry current;
    if (!remote_info(entry.path, current, error)) return false;
    if (current.size != entry.size || (!entry.identity.empty() && current.identity != entry.identity)) {
        error = "远程文件在列表后已变化"; return false;
    }
    total = current.size;
    std::string share, path; bool root = false;
    if (!resolve_path(entry.path, share, path, root, error) || root) return false;
    if (!ensure_share(share, error)) return false;

    std::uint64_t offset = 0;
    if (FILE* existing = std::fopen(local_path.c_str(), "rb")) {
        std::fseek(existing, 0, SEEK_END);
        const long pos = std::ftell(existing);
        if (pos > 0) offset = static_cast<std::uint64_t>(pos);
        std::fclose(existing);
    }
    if (offset > total) offset = 0;
    FILE* out = std::fopen(local_path.c_str(), offset ? "ab" : "wb");
    if (!out) { error = "cannot open local cache file"; return false; }
    smb2fh* in = smb2_open(ctx_, path.c_str(), O_RDONLY);
    if (!in) { std::fclose(out); error = smb2_get_error(ctx_); return false; }

    transferred = offset;
    const RemoteStreamDataCallback writer = [&](const unsigned char* data, std::size_t size, std::uint64_t logical) {
        (void)logical;
        if (std::fwrite(data, 1, size, out) != size) return false;
        transferred += static_cast<std::uint64_t>(size);
        return true;
    };
    const TransferProgressCallback relative_progress = [&](std::uint64_t done, std::uint64_t) {
        return !progress || progress(offset + done, total);
    };
    std::uint64_t delivered = 0, requests = 0;
    const auto pipeline_request = std::min<std::uint32_t>(max_read_, kSmbPipelineRequestCap);
    const bool ok = pipeline_read(ctx_, in, offset, total - offset, pipeline_request, writer,
                                  relative_progress, delivered, requests, error, kSmbPipelineWindow);
    const bool synced = std::fflush(out) == 0 && ::fsync(fileno(out)) == 0;
    smb2_close(ctx_, in);
    const bool closed = std::fclose(out) == 0;
    if (!ok) {
        if (error == "SMB 数据消费者已停止") error = "local write failed";
        return false;
    }
    if (!synced || !closed) { error = "failed to sync local cache file"; return false; }
    if (transferred != total) { error = "download ended before remote EOF"; return false; }
    RemoteDirEntry completed;
    if (!remote_info(entry.path, completed, error)) return false;
    if (completed.size != current.size || completed.identity != current.identity) {
        error = "remote file changed while it was downloading"; return false;
    }
    return true;
}

bool SmbRemoteClient::prepare_direct_read(const RemoteDirEntry& entry, std::string& error) {
    close_direct_read(); error.clear();
    if (entry.is_dir || entry.size == 0 || entry.identity.rfind("smb:", 0) != 0) {
        error = "SMB 网络直装需要稳定的文件身份"; return false;
    }
    RemoteDirEntry current;
    if (!remote_info(entry.path, current, error)) return false;
    if (current.size != entry.size || current.identity != entry.identity) {
        error = "远程文件在列表后已变化"; return false;
    }
    std::string share, path; bool root = false;
    if (!resolve_path(entry.path, share, path, root, error) || root) {
        if (root) error = "SMB 服务器根目录不是文件";
        return false;
    }
    if (!ensure_share(share, error)) return false;
    smb2fh* file = smb2_open(ctx_, path.c_str(), O_RDONLY);
    if (!file) { error = smb2_get_error(ctx_); return false; }
    direct_file_ = file;
    direct_entry_ = entry;
    direct_share_ = share;
    direct_path_ = path;
    return true;
}

bool SmbRemoteClient::reopen_direct_file(std::string& error) {
    if (direct_entry_.path.empty() || direct_share_.empty()) {
        error = "SMB 网络直读会话尚未准备"; return false;
    }
    reset();
    if (!ensure_share(direct_share_, error)) return false;
    RemoteDirEntry current;
    if (!remote_info(direct_entry_.path, current, error)) return false;
    if (current.size != direct_entry_.size || current.identity != direct_entry_.identity) {
        error = "SMB 网络直装期间远程文件已变化"; return false;
    }
    if (!ensure_share(direct_share_, error)) return false;
    direct_file_ = smb2_open(ctx_, direct_path_.c_str(), O_RDONLY);
    if (!direct_file_) {
        error = smb2_get_error(ctx_) ? smb2_get_error(ctx_) : "SMB 网络直读重新打开文件失败";
        return false;
    }
    return true;
}

bool SmbRemoteClient::read_range(const RemoteDirEntry& entry, std::uint64_t offset,
                                 void* buffer, std::size_t size, std::size_t& actual,
                                 std::string& error) {
    actual = 0; error.clear();
    if (!buffer && size != 0) { error = "SMB 网络直读缓冲区为空"; return false; }
    if (!direct_file_ || entry.path != direct_entry_.path || entry.identity != direct_entry_.identity ||
        entry.size != direct_entry_.size) {
        error = "SMB 网络直读会话未为该文件准备"; return false;
    }
    if (offset > entry.size || size > entry.size - offset) { error = "SMB 网络直读范围超出文件"; return false; }
    auto* out = static_cast<std::uint8_t*>(buffer);
    int failures = 0;
    while (actual < size) {
        const auto want = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            std::min<std::size_t>(size - actual, max_read_), entry.size - (offset + actual)));
        const int got = smb2_pread(ctx_, direct_file_, out + actual, want, offset + actual);
        if (got > 0) { actual += static_cast<std::size_t>(got); continue; }
        if (got == 0) { error = "SMB 网络直读在远程文件结尾前提前结束"; return false; }
        error = smb2_get_error(ctx_) ? smb2_get_error(ctx_) : "SMB 网络直读失败";
        if (failures++ >= direct_retries_) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(failures == 1 ? 150 : 350));
        std::string reopen_error;
        if (!reopen_direct_file(reopen_error)) { error += "; 重连失败: " + reopen_error; return false; }
    }
    // Keep identity checks at direct-stream boundaries/reconnect, not after every
    // 8 MiB read-ahead slot. The same open SMB handle anchors the file in between.
    return true;
}

bool SmbRemoteClient::stream_range(const RemoteDirEntry& entry, std::uint64_t offset,
                                   std::uint64_t size, const RemoteStreamDataCallback& data,
                                   const TransferProgressCallback& progress, std::string& error) {
    if (!direct_file_ || entry.path != direct_entry_.path || entry.identity != direct_entry_.identity ||
        entry.size != direct_entry_.size) {
        error = "SMB 网络直读会话未为该文件准备"; return false;
    }
    if (offset > entry.size || size > entry.size - offset) { error = "SMB 连续流范围超出文件"; return false; }
    std::uint64_t delivered = 0, requests = 0;
    const auto pipeline_request = std::min<std::uint32_t>(max_read_, kSmbPipelineRequestCap);
    return pipeline_read(ctx_, direct_file_, offset, size, pipeline_request, data, progress,
                         delivered, requests, error, kSmbPipelineWindow);
}

bool SmbRemoteClient::finish_direct_read(const RemoteDirEntry& entry, std::string& error) {
    error.clear();
    RemoteDirEntry current;
    const bool ok = direct_file_ && entry.path == direct_entry_.path &&
                    remote_info(entry.path, current, error) &&
                    current.size == entry.size && current.identity == entry.identity;
    if (!ok && error.empty()) error = "SMB 网络直装期间远程文件已变化";
    close_direct_read();
    return ok;
}

void SmbRemoteClient::close_direct_read() {
    if (ctx_ && direct_file_) smb2_close(ctx_, direct_file_);
    direct_file_ = nullptr;
    direct_entry_ = {};
    direct_share_.clear();
    direct_path_.clear();
}

bool SmbRemoteClient::benchmark_detailed(const std::string& remote_path, std::size_t bytes,
                                         RemoteBenchmarkStats& stats, std::string& error) {
    stats = {};
    RemoteDirEntry entry;
    if (!remote_info(remote_path, entry, error)) return false;
    const std::uint64_t target = std::min<std::uint64_t>(entry.size, bytes);
    if (!target) { error = "empty file"; return false; }
    std::string share, path; bool root = false;
    if (!resolve_path(remote_path, share, path, root, error) || root) return false;
    if (!ensure_share(share, error)) return false;
    smb2fh* in = smb2_open(ctx_, path.c_str(), O_RDONLY);
    if (!in) { error = smb2_get_error(ctx_); return false; }

    std::uint64_t delivered = 0, requests = 0;
    const auto start = std::chrono::steady_clock::now();
    const auto pipeline_request = std::min<std::uint32_t>(max_read_, kSmbPipelineRequestCap);
    const bool ok = pipeline_read(ctx_, in, 0, target, pipeline_request, {}, {}, delivered, requests, error,
                                  kSmbPipelineWindow);
    const auto end = std::chrono::steady_clock::now();
    const std::uint32_t recv_buffer = socket_recv_buffer(smb2_get_fd(ctx_));
    smb2_close(ctx_, in);
    if (!ok) return false;
    const double seconds = std::chrono::duration<double>(end - start).count();
    if (seconds <= 0.0 || delivered == 0) { error = "benchmark timer/read error"; return false; }
    stats.bytes = delivered;
    stats.seconds = seconds;
    stats.mib_per_sec = (static_cast<double>(delivered) / (1024.0 * 1024.0)) / seconds;
    stats.request_count = requests;
    stats.average_request_bytes = requests ? delivered / requests : 0;
    stats.negotiated_read_size = max_read_;
    stats.pipeline_request_size = pipeline_request;
    stats.pipeline_window = static_cast<std::uint32_t>(kSmbPipelineWindow);
    stats.socket_recv_buffer = recv_buffer;
    return true;
}
