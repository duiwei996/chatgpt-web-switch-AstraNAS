// SPDX-License-Identifier: GPL-3.0-or-later
#include "actions.hpp"
#ifdef __SWITCH__
#include "util.hpp"
#include "../download_cache.hpp"
#include "../network_runtime.hpp"
#include "../package_inspect.hpp"
#include "../remote/remote_package_source.hpp"
#include "../sha256.hpp"
#include "../title_backend/buffered_stream.hpp"
#include "../title_backend/package_source.hpp"
#include "../ui/dialogs.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace astranas::app {
namespace {
constexpr auto kInputPollInterval = std::chrono::milliseconds(25);
constexpr auto kUiDrawInterval = std::chrono::milliseconds(150);

void draw_transfer_progress(ActionContext& ctx, const RemoteDirEntry& entry, const std::string& protocol,
                            std::uint64_t done, std::uint64_t total, double mib_per_sec,
                            int attempt, int max_attempts, const char* phase,
                            const std::string& extra_detail = {}) {
    std::string detail = "来源：" + protocol + "  ·  " + entry.path;
    if (!extra_detail.empty()) detail += "\n" + extra_detail;
    astranas::ui::draw_progress_page(ctx.gui, phase, entry.name, detail,
                                    done, total, mib_per_sec, attempt, max_attempts);
}
bool poll_cancel(ActionContext& ctx, bool& cancelled) {
    if (!appletMainLoop()) { cancelled = true; return true; }
    padUpdate(&ctx.pad);
    const u64 pressed = padGetButtonsDown(&ctx.pad) | padGetButtons(&ctx.pad);
    if (pressed & HidNpadButton_Plus) { ctx.exit_requested = true; cancelled = true; return true; }
    if (pressed & HidNpadButton_B) { cancelled = true; return true; }
    return false;
}
std::string remote_parent(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return "/";
    return path.substr(0, slash);
}
std::string lower_path(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
const char* package_extension(PackageContainerKind kind) {
    switch (kind) {
        case PackageContainerKind::Nsp: return ".nsp";
        case PackageContainerKind::Nsz: return ".nsz";
        case PackageContainerKind::Xci: return ".xci";
        case PackageContainerKind::Xcz: return ".xcz";
        default: return ".pkg";
    }
}

bool supports_fast_download(RemoteClient& remote, const RemoteDirEntry& entry) {
    if (entry.is_dir || entry.size == 0) return false;
    const std::string protocol = remote.protocol_name();
    if (protocol == "SMB2/3") return entry.identity.rfind("smb:", 0) == 0;
    if (protocol == "WebDAV" || protocol == "WebDAVS")
        return entry.identity.rfind("etag:", 0) == 0 && entry.identity.size() > 5;
    return false;
}

bool download_fast_stream(RemoteClient& remote, const RemoteDirEntry& entry,
                          const std::string& local_path, std::uint64_t start_bytes,
                          std::uint64_t& transferred, std::uint64_t& total,
                          ActionContext& ctx, int attempt, int max_attempts,
                          const char* phase, bool& cancelled, std::string& error) {
    error.clear();
    cancelled = false;
    transferred = start_bytes;
    total = entry.size;
    if (start_bytes > entry.size) { error = "本地断点超过远程文件大小"; return false; }

    astranas::remote::RemotePackageSource source(remote, entry);
    if (!source.open(entry.path, error)) return false;

    std::FILE* out = std::fopen(local_path.c_str(), start_bytes ? "ab" : "wb");
    if (!out) { error = "cannot open local cache file"; return false; }
    (void)std::setvbuf(out, nullptr, _IOFBF, 1024u * 1024u);

    const auto start_time = std::chrono::steady_clock::now();
    auto last_poll = start_time - std::chrono::seconds(1);
    auto last_draw = start_time - std::chrono::seconds(1);
    std::uint64_t written = 0;
    std::uint64_t write_ns = 0;
    astranas::title_backend::BufferedStreamTelemetry telemetry;

    auto update_progress = [&](std::uint64_t done, bool force_draw) -> bool {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_poll >= kInputPollInterval || force_draw) {
            if (poll_cancel(ctx, cancelled)) return false;
            last_poll = now;
        }
        if (now - last_draw < kUiDrawInterval && !force_draw && done < total) return true;

        const double seconds = std::max(0.001, std::chrono::duration<double>(now - start_time).count());
        const std::uint64_t done_delta = done >= start_bytes ? done - start_bytes : 0;
        const std::uint64_t produced = telemetry.produced.load(std::memory_order_relaxed);
        const std::uint64_t consumed_relative = telemetry.consumed.load(std::memory_order_relaxed);
        const std::uint64_t buffered = produced > consumed_relative ? produced - consumed_relative : 0;
        const std::uint64_t active_read_ns = telemetry.active_read_ns.load(std::memory_order_relaxed);
        const std::uint64_t backpressure_ns = telemetry.backpressure_ns.load(std::memory_order_relaxed);
        const double overall = (static_cast<double>(done_delta) / (1024.0 * 1024.0)) / seconds;
        const double source_seconds = static_cast<double>(active_read_ns) / 1000000000.0;
        const double source_speed = source_seconds > 0.0
            ? (static_cast<double>(produced) / (1024.0 * 1024.0)) / source_seconds : 0.0;
        const double write_seconds = static_cast<double>(write_ns) / 1000000000.0;
        const double write_speed = write_seconds > 0.0
            ? (static_cast<double>(written) / (1024.0 * 1024.0)) / write_seconds : 0.0;

        std::ostringstream layers;
        layers.setf(std::ios::fixed);
        layers.precision(1);
        layers << "高速连续流 · 网络纯读 " << source_speed << " MiB/s"
               << " · 本机写入 " << write_speed << " MiB/s"
               << " · 等待落盘 " << (backpressure_ns / 1000000.0) << " ms"
               << " · RAM " << (buffered / 1024.0 / 1024.0) << "/"
               << (telemetry.capacity_bytes / 1024.0 / 1024.0) << " MiB";
        draw_transfer_progress(ctx, entry, remote.protocol_name(), done, total, overall,
                               attempt, max_attempts, phase, layers.str());
        last_draw = now;
        return true;
    };

    bool streamed = true;
    std::string stream_error;
    try {
        if (start_bytes < total) {
            streamed = astranas::title_backend::stream_buffered_read_ahead(
                source, start_bytes, total - start_bytes,
                [&](const unsigned char* data, std::size_t size, std::uint64_t logical_offset) {
                    std::size_t position = 0;
                    while (position < size) {
                        const std::size_t slice = std::min(
                            astranas::title_backend::kDownloadWriteSliceSize, size - position);
                        const auto write_begin = std::chrono::steady_clock::now();
                        const std::size_t put = std::fwrite(data + position, 1, slice, out);
                        const auto write_end = std::chrono::steady_clock::now();
                        write_ns += static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(write_end - write_begin).count());
                        if (put != slice) throw std::runtime_error("local write failed");
                        position += slice;
                        written += slice;
                        transferred = start_bytes + logical_offset + position;
                        if (!update_progress(transferred, transferred >= total))
                            throw std::runtime_error("cancelled");
                    }
                }, stream_error,
                [&](std::uint64_t consumed, std::uint64_t) {
                    return update_progress(start_bytes + consumed, false);
                }, astranas::title_backend::kInstallStreamChunkSize,
                astranas::title_backend::kDownloadReadAheadSlots,
                false, &telemetry);
        }
    } catch (const std::exception& ex) {
        stream_error = ex.what();
        streamed = false;
    }

    bool sync_ok = true;
    if (streamed) sync_ok = std::fflush(out) == 0 && ::fsync(fileno(out)) == 0;
    const bool close_ok = std::fclose(out) == 0;
    if (!streamed) {
        if (lower_copy(stream_error).find("cancel") != std::string::npos) cancelled = true;
        error = stream_error.empty() ? "高速连续流下载未完成" : stream_error;
        return false;
    }
    if (!sync_ok || !close_ok) { error = "failed to sync local cache file"; return false; }
    if (!source.finish(error)) return false;
    transferred = total;
    update_progress(total, true);
    return true;
}
} // namespace

bool transfer_remote_file(RemoteClient& remote, const RemoteDirEntry& entry, const AppConfig& config,
                          const std::string& destination, ActionContext& ctx,
                          std::string& status, const char* phase) {
    if (!remote_path_within(config.root, entry.path) && !(config.protocol == "smb" && config.share.empty())) {
        status = "已阻止访问配置远程范围之外的路径";
        return false;
    }
    if (!local_path_is_within(config.local_root, destination) &&
        !local_path_is_within(config.cache_dir, destination)) {
        status = "已阻止写入当前存储设备之外的位置";
        return false;
    }
    const auto slash = destination.find_last_of('/');
    if (slash != std::string::npos && !local_mkdir_p(destination.substr(0, slash))) {
        status = "无法创建目标目录";
        return false;
    }

    const std::string object_key = remote_object_key(config, entry);
    const std::string partial = destination + ".astranas-part";
    const std::string partial_meta_path = partial + ".astranas-meta";
    const std::string final_meta_path = destination + ".astranas-meta";
    if (local_path_exists(destination)) {
        TransferMetadata completed_metadata;
        bool reusable = !entry.identity.empty() && read_transfer_metadata(final_meta_path, completed_metadata) &&
                        completed_metadata.object_key == object_key && (!entry.size || local_file_size(destination) == entry.size);
        if (reusable && config.verify_sha256) {
            reusable = is_valid_sha256_hex(completed_metadata.sha256);
            if (reusable) {
                std::string digest, hash_error;
                reusable = sha256_file(destination, digest, hash_error) && digest == completed_metadata.sha256;
                if (!hash_error.empty()) append_debug_log("检查已完成文件", hash_error);
            }
        }
        if (reusable) { status = std::string(phase) + "已完成，可直接使用：" + basename_of(destination); return true; }
        status = "本机当前目录已有同名文件，且无法确认来自同一个远端对象；为避免覆盖，请先重命名、移动或删除该文件：" +
                 basename_of(destination);
        return false;
    }

    TransferMetadata partial_metadata;
    if (!read_transfer_metadata(partial_meta_path, partial_metadata) || partial_metadata.object_key != object_key) {
        std::remove(partial.c_str());
        std::remove(partial_meta_path.c_str());
        partial_metadata = TransferMetadata{object_key, {}};
        std::string metadata_error;
        if (!write_transfer_metadata(partial_meta_path, partial_metadata, metadata_error)) {
            status = friendly_error("无法保存断点续传信息", metadata_error); return false;
        }
    }

    const int max_attempts = std::max(1, config.download_retries + 1);
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        const std::uint64_t start_bytes = local_file_size(partial);
        const auto start_time = std::chrono::steady_clock::now();
        auto last_poll = start_time - std::chrono::seconds(1);
        auto last_draw = start_time - std::chrono::seconds(1);
        bool cancelled = false;
        std::uint64_t transferred = start_bytes;
        std::uint64_t total = entry.size;
        std::string error;
        bool ok = false;
        {
            astranas::network::ScopedCpuBoost network_boost;
            if (supports_fast_download(remote, entry)) {
                ok = download_fast_stream(remote, entry, partial, start_bytes, transferred, total,
                                          ctx, attempt, max_attempts, phase, cancelled, error);
            } else {
                const DownloadProgressCallback progress = [&](std::uint64_t done, std::uint64_t remote_total) {
                    transferred = done;
                    if (remote_total > 0) total = remote_total;
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_poll >= kInputPollInterval) {
                        if (poll_cancel(ctx, cancelled)) return false;
                        last_poll = now;
                    }
                    if (now - last_draw >= kUiDrawInterval || (total > 0 && done >= total)) {
                        const double seconds = std::max(0.001, std::chrono::duration<double>(now - start_time).count());
                        const std::uint64_t delta = done >= start_bytes ? done - start_bytes : 0;
                        const double speed = (static_cast<double>(delta) / (1024.0 * 1024.0)) / seconds;
                        draw_transfer_progress(ctx, entry, remote.protocol_name(), done, total, speed,
                                               attempt, max_attempts, phase, "兼容下载路径");
                        last_draw = now;
                    }
                    return true;
                };
                ok = remote.download_responsive(entry, partial, transferred, total, progress, error);
            }
        }
        if (ok) {
            const auto final_size = local_file_size(partial);
            if (entry.size && final_size != entry.size) {
                status = "传输结束，但文件大小不一致；已保留临时文件以便排查";
                append_debug_log("文件大小不一致", entry.path); return false;
            }
            if (config.verify_sha256) {
                std::string digest, hash_error;
                bool hash_cancelled = false;
                const bool hash_ok = sha256_file(partial, digest, hash_error,
                    [&](std::uint64_t done, std::uint64_t hash_total) {
                        if (poll_cancel(ctx, hash_cancelled)) return false;
                        draw_transfer_progress(ctx, entry, remote.protocol_name(), done, hash_total, 0.0,
                                               attempt, max_attempts, "正在校验 SHA-256");
                        return true;
                    });
                if (!hash_ok) {
                    status = hash_cancelled ? "SHA-256 校验已取消，临时文件已保留" : friendly_error("SHA-256 校验失败", hash_error);
                    return false;
                }
                partial_metadata.sha256 = digest;
                std::string metadata_error;
                if (!write_transfer_metadata(partial_meta_path, partial_metadata, metadata_error)) {
                    status = friendly_error("无法保存校验结果", metadata_error); return false;
                }
            }
            if (std::rename(partial.c_str(), destination.c_str()) != 0) {
                status = friendly_error("无法发布已完成的文件", std::strerror(errno)); return false;
            }
            std::remove(final_meta_path.c_str());
            if (std::rename(partial_meta_path.c_str(), final_meta_path.c_str()) != 0) {
                std::string metadata_error;
                write_transfer_metadata(final_meta_path, partial_metadata, metadata_error);
                if (!metadata_error.empty()) append_debug_log("补写传输信息", metadata_error);
            }
            status = std::string(phase) + "完成：" + basename_of(destination) + "（" + format_size(final_size) + "）";
            return true;
        }
        if (cancelled || lower_copy(error).find("cancel") != std::string::npos) {
            status = std::string(phase) + (ctx.exit_requested ? "已停止，正在退出" : "已取消，临时文件已保留，可下次继续");
            return false;
        }
        if (lower_copy(error).find("changed") != std::string::npos || error.find("发生变化") != std::string::npos) {
            std::remove(partial.c_str()); std::remove(partial_meta_path.c_str());
            status = "远端文件已发生变化，请刷新当前文件夹后重试"; append_debug_log("远端文件变化", error); return false;
        }
        if (attempt < max_attempts) {
            std::string reconnect_error;
            if (!remote.connect(config, reconnect_error)) append_debug_log("自动重连", reconnect_error);
        } else status = friendly_error(std::string(phase) + "失败", error);
    }
    return false;
}
bool upload_local_file(RemoteClient& remote, const LocalEntry& entry, const AppConfig& config,
                       const std::string& remote_directory, ActionContext& ctx,
                       std::string& status) {
    if (entry.is_dir) {
        status = "暂不支持上传文件夹，请选择一个文件";
        return false;
    }
    if (!local_path_is_within(config.local_root, entry.path) || !local_path_exists(entry.path)) {
        status = "已阻止上传当前本机存储范围之外的文件";
        return false;
    }
    if (config.protocol == "smb" && config.share.empty() && remote_directory == "/") {
        status = "SMB 服务器根目录不能直接写文件，请先进入一个共享目录";
        return false;
    }
    const std::string bound = config.protocol == "smb" && config.share.empty() ? "/" : config.root;
    const std::string target = join_remote_path(remote_directory, entry.name);
    if (!remote_path_within(bound, target)) {
        status = "已阻止上传到配置远程范围之外的位置";
        return false;
    }

    std::vector<RemoteDirEntry> current_entries;
    std::string list_error;
    if (!remote.list_dir(remote_directory, current_entries, list_error)) {
        status = friendly_error("无法检查 NAS 上传目录", list_error);
        return false;
    }
    const auto existing = std::find_if(current_entries.begin(), current_entries.end(), [&](const auto& item) {
        return item.name == entry.name;
    });
    if (existing != current_entries.end() && existing->is_dir) {
        status = "NAS 当前目录已有同名文件夹，无法上传该文件";
        return false;
    }
    const bool overwrite = existing != current_entries.end();
    const std::uint64_t source_size = local_file_size(entry.path);
    const std::string detail = "本机文件：" + entry.name + "\n大小：" + format_size(source_size) +
        "\nNAS 目标：" + target +
        (overwrite ? "\nNAS 已有同名文件，将覆盖原文件。" : "");
    if (!astranas::ui::confirm_action(ctx.gui, ctx.pad, ctx.exit_requested,
            overwrite ? "覆盖 NAS 文件" : "上传文件到 NAS", detail,
            overwrite ? "确认覆盖" : "开始上传", "取消")) {
        status = ctx.exit_requested ? "正在退出" : "上传已取消";
        return false;
    }

    const int max_attempts = std::max(1, config.download_retries + 1);
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        const auto start_time = std::chrono::steady_clock::now();
        auto last_draw = start_time - std::chrono::seconds(1);
        bool cancelled = false;
        std::uint64_t transferred = 0;
        std::uint64_t total = source_size;
        std::string error;
        const UploadProgressCallback progress = [&](std::uint64_t done, std::uint64_t remote_total) {
            transferred = done;
            if (remote_total > 0) total = remote_total;
            if (poll_cancel(ctx, cancelled)) return false;
            const auto now = std::chrono::steady_clock::now();
            if (now - last_draw >= std::chrono::milliseconds(150) || (total > 0 && done >= total)) {
                const double seconds = std::max(0.001, std::chrono::duration<double>(now - start_time).count());
                const double speed = (static_cast<double>(done) / (1024.0 * 1024.0)) / seconds;
                astranas::ui::draw_progress_page(ctx.gui, "正在上传到 NAS", entry.name,
                    "目标：" + remote.protocol_name() + "  ·  " + target,
                    done, total, speed, attempt, max_attempts);
                last_draw = now;
            }
            return true;
        };
        bool upload_ok = false;
        {
            astranas::network::ScopedCpuBoost network_boost;
            upload_ok = remote.upload(entry.path, target, transferred, total, progress, error);
        }
        if (upload_ok) {
            std::vector<RemoteDirEntry> verify_entries;
            std::string verify_error;
            if (!remote.list_dir(remote_directory, verify_entries, verify_error)) {
                status = friendly_error("文件已上传，但 NAS 回读校验失败", verify_error);
                return false;
            }
            const auto uploaded = std::find_if(verify_entries.begin(), verify_entries.end(), [&](const auto& item) {
                return !item.is_dir && item.name == entry.name;
            });
            if (uploaded == verify_entries.end() || uploaded->size != total) {
                status = "文件已上传，但 NAS 回读大小不一致，请检查目标文件";
                append_debug_log("NAS 上传回读校验", target);
                return false;
            }
            status = "上传完成：" + entry.name + " -> " + target + "（" + format_size(total) + "）";
            return true;
        }
        if (cancelled || lower_copy(error).find("cancel") != std::string::npos) {
            status = "上传已取消；NAS 上可能保留未完成的同名文件";
            return false;
        }
        if (attempt < max_attempts) {
            std::string reconnect_error;
            if (!remote.connect(config, reconnect_error)) append_debug_log("上传自动重连", reconnect_error);
        } else {
            status = friendly_error("上传到 NAS 失败", error);
        }
    }
    return false;
}

bool stage_remote_candidate(RemoteClient& remote, const RemoteDirEntry& entry,
                            const AppConfig& config, ActionContext& ctx,
                            std::string& status, std::string& staged_path) {
    staged_path.clear();
    if (entry.is_dir) {
        const auto kind = detect_package_container(entry.path);
        if (kind != PackageContainerKind::Nsp && kind != PackageContainerKind::Nsz &&
            kind != PackageContainerKind::Xci && kind != PackageContainerKind::Xcz) {
            status = "所选远端文件夹不是可识别的分卷安装包"; return false;
        }
        std::vector<RemoteDirEntry> directory_entries;
        std::string list_error;
        if (!remote.list_dir(entry.path, directory_entries, list_error)) {
            status = friendly_error("无法读取分卷安装包目录", list_error); return false;
        }
        std::vector<std::pair<std::uint64_t, RemoteDirEntry>> parts;
        for (const auto& candidate : directory_entries) {
            if (candidate.is_dir || candidate.name.empty() ||
                !std::all_of(candidate.name.begin(), candidate.name.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) continue;
            std::uint64_t index = 0;
            for (const char c : candidate.name) {
                if (index > 999999999ull) { status = "分卷编号异常，已停止处理"; return false; }
                index = index * 10 + static_cast<unsigned>(c - '0');
            }
            parts.emplace_back(index, candidate);
        }
        std::sort(parts.begin(), parts.end(), [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
        if (parts.empty()) { status = "分卷目录中没有找到数字编号的分卷文件"; return false; }
        for (std::size_t i = 0; i < parts.size(); ++i) if (parts[i].first != i) { status = "分卷安装包存在缺失或重复的分卷"; return false; }
        staged_path = local_join_path(config.cache_dir, remote_cache_filename(config, entry));
        if (!local_mkdir_p(staged_path)) { status = "无法创建分卷安装包缓存目录"; return false; }
        for (const auto& part : parts) {
            if (!transfer_remote_file(remote, part.second, config, local_join_path(staged_path, part.second.name), ctx, status, "正在暂存分卷")) return false;
        }
        return true;
    }

    astranas::title_backend::SplitPackagePartInfo selected_part;
    if (!astranas::title_backend::describe_split_package_part(entry.path, selected_part)) {
        staged_path = local_join_path(config.cache_dir, remote_cache_filename(config, entry));
        return transfer_remote_file(remote, entry, config, staged_path, ctx, status, "正在暂存安装文件");
    }
    if (selected_part.index != 0) { status = "请选择分卷安装包的第一个分卷文件"; return false; }
    std::vector<RemoteDirEntry> directory_entries;
    std::string list_error;
    const std::string parent = remote_parent(entry.path);
    if (!remote.list_dir(parent, directory_entries, list_error)) { status = friendly_error("无法读取分卷文件", list_error); return false; }
    std::vector<std::pair<std::uint64_t, RemoteDirEntry>> parts;
    const std::string expected_prefix = lower_path(selected_part.prefix);
    for (const auto& candidate : directory_entries) {
        if (candidate.is_dir) continue;
        astranas::title_backend::SplitPackagePartInfo part;
        if (astranas::title_backend::describe_split_package_part(candidate.path, part) && lower_path(part.prefix) == expected_prefix)
            parts.emplace_back(part.index, candidate);
    }
    std::sort(parts.begin(), parts.end(), [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    if (parts.empty()) { status = "没有找到同一安装包的分卷文件"; return false; }
    for (std::size_t i = 0; i < parts.size(); ++i) if (parts[i].first != i) { status = "分卷安装包存在缺失或重复的分卷"; return false; }
    const auto kind = detect_package_container(entry.path);
    staged_path = local_join_path(config.cache_dir, remote_cache_filename(config, entry) + ".split" + package_extension(kind));
    if (!local_mkdir_p(staged_path)) { status = "无法创建分卷安装包缓存目录"; return false; }
    const std::size_t width = std::max<std::size_t>(2, selected_part.width);
    for (const auto& part : parts) {
        std::ostringstream name;
        name << std::setw(static_cast<int>(width)) << std::setfill('0') << part.first;
        if (!transfer_remote_file(remote, part.second, config, local_join_path(staged_path, name.str()), ctx, status, "正在暂存分卷")) return false;
    }
    return true;
}

} // namespace astranas::app
#endif
