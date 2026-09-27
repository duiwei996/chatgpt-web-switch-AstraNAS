// SPDX-License-Identifier: GPL-3.0-or-later
#include "actions.hpp"
#ifdef __SWITCH__
#include "constants.hpp"
#include "util.hpp"
#include "../completion_sound.hpp"
#include "../install_cleanup.hpp"
#include "../install_performance.hpp"
#include "../network_runtime.hpp"
#include "../package_inspect.hpp"
#include "../remote/remote_package_source.hpp"
#include "../batch_install_order.hpp"
#include "../ui/dialogs.hpp"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <utility>

namespace astranas::app {
namespace {
using astranas::ui::Gui;
namespace palette = astranas::ui::palette;
const char* container_label(PackageContainerKind kind) {
    switch (kind) {
        case PackageContainerKind::HomebrewNro: return "NRO 自制程序";
        case PackageContainerKind::Nsp: return "NSP 安装包";
        case PackageContainerKind::Nsz: return "NSZ 压缩安装包";
        case PackageContainerKind::Xci: return "XCI 游戏卡镜像";
        case PackageContainerKind::Xcz: return "XCZ 压缩游戏卡镜像";
        default: return "未知格式";
    }
}
bool poll_cancel(ActionContext& ctx, bool& cancelled) {
    if (!appletMainLoop()) { cancelled = true; return true; }
    padUpdate(&ctx.pad);
    const u64 pressed = padGetButtonsDown(&ctx.pad) | padGetButtons(&ctx.pad);
    if (pressed & HidNpadButton_Plus) { ctx.exit_requested = true; cancelled = true; return true; }
    if (pressed & HidNpadButton_B) { cancelled = true; return true; }
    return false;
}
const char* content_kind_text(astranas::user_backend::ContentKind kind) {
    using astranas::user_backend::ContentKind;
    switch (kind) {
        case ContentKind::Application: return "基础内容";
        case ContentKind::Patch: return "更新内容";
        case ContentKind::AddOnContent: return "DLC 内容";
        case ContentKind::DataPatch: return "数据更新";
        default: return "其他内容";
    }
}
const char* relation_text(astranas::user_backend::InstalledRelation relation) {
    using astranas::user_backend::InstalledRelation;
    switch (relation) {
        case InstalledRelation::Upgrade: return "可升级";
        case InstalledRelation::SameVersion: return "版本相同";
        case InstalledRelation::Downgrade: return "将降级";
        default: return "未安装";
    }
}
std::string firmware_text(std::uint32_t value) {
    if (value == 0) return "无要求";
    std::ostringstream out;
    out << (value >> 26) << '.' << ((value >> 20) & 0x3f) << '.' << ((value >> 16) & 0xf)
        << "（" << value << "）";
    return out.str();
}
std::string install_storage_text(astranas::user_backend::InstallStorage storage) {
    return storage == astranas::user_backend::InstallStorage::NandUser ? "主机存储（NAND）" : "SD 卡";
}
astranas::user_backend::PreflightDecision choose_preflight(
        ActionContext& ctx, const std::string& source_path,
        const astranas::user_backend::PackageInstallInfo& info,
        bool interactive, bool delete_after_success) {
    using astranas::user_backend::InstalledRelation;
    using astranas::user_backend::PreflightDecision;
    const bool has_downgrade = std::any_of(info.contents.begin(), info.contents.end(), [](const auto& content) {
        return content.relation == InstalledRelation::Downgrade;
    });
    const bool has_actionable = std::any_of(info.contents.begin(), info.contents.end(), [](const auto& content) {
        return content.relation == InstalledRelation::NotInstalled || content.relation == InstalledRelation::Upgrade;
    });
    const bool requires_force = has_downgrade || !has_actionable;
    if (!interactive) return astranas::user_backend::safe_batch_preflight_decision(info);

    Gui& gui = ctx.gui;
    gui.begin();
    gui.clear(palette::background);
    gui.fill_rect(0, 0, Gui::kWidth, 70, palette::navy);
    gui.text(40, 16, 28, palette::white, "安装确认");
    gui.round_rect(48, 92, 1184, 550, 16, palette::surface);
    gui.stroke_rect(48, 92, 1184, 550, 1, palette::border);
    gui.text(78, 120, 23, palette::text, basename_of(source_path), 760);
    gui.text(78, 158, 16, palette::muted,
             "格式：" + info.container + "  ·  大小：" + format_size(info.source_size) +
             "  ·  目标：" + install_storage_text(info.storage), 1100);
    if (!info.title_name.empty()) gui.text(78, 190, 19, palette::text, "应用：" + info.title_name, 1100);
    gui.text(78, 226, 16, palette::muted,
             "新增占用：" + format_size(info.required_space) + "  ·  可用空间：" + format_size(info.free_space), 1100);
    const std::size_t shown = std::min<std::size_t>(info.contents.size(), 5);
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& content = info.contents[i];
        const int y = 272 + static_cast<int>(i) * 55;
        const bool danger = content.relation == InstalledRelation::Downgrade;
        gui.round_rect(72, y, 1110, 47, 8, danger ? palette::warning_soft : palette::surface_alt);
        gui.text(88, y + 7, 16, danger ? palette::warning : palette::text,
                 std::string(content_kind_text(content.kind)) + "  " + title_id_text(content.title_id), 570);
        gui.text(666, y + 7, 15, palette::muted,
                 "v" + std::to_string(content.version) + "  ·  " + format_size(content.install_size) +
                 "  ·  " + relation_text(content.relation), 500);
        const std::string requirement =
            (content.kind == astranas::user_backend::ContentKind::AddOnContent ||
             content.kind == astranas::user_backend::ContentKind::DataPatch)
                ? "要求应用版本：" + std::to_string(content.required_system_version)
                : "固件要求：" + firmware_text(content.required_system_version);
        gui.text(666, y + 26, 13, palette::muted, requirement, 500);
    }
    if (shown < info.contents.size())
        gui.text(82, 557, 15, palette::muted, "另有 " + std::to_string(info.contents.size() - shown) + " 条内容记录未展开");
    gui.text(82, 589, 15, palette::muted,
             std::string("安装成功后删除源文件：") + (delete_after_success ? "是" : "否"));
    if (requires_force) {
        gui.text(82, 615, 14, palette::warning,
                 has_downgrade ? "检测到降级内容：默认跳过，只有明确需要降级时才选择强制安装。"
                               : "当前安装包没有可升级的新内容：默认跳过，如需重装可选择强制安装。", 1090);
        gui.button_hint(64, 666, "A", "跳过");
        gui.button_hint(282, 666, "X", "强制安装", palette::warning);
        gui.button_hint(570, 666, "B", "取消", palette::danger);
    } else {
        gui.button_hint(64, 666, "A", "开始安装");
        gui.button_hint(318, 666, "B", "取消", palette::danger);
    }
    gui.end();

    while (appletMainLoop()) {
        padUpdate(&ctx.pad);
        const u64 down = padGetButtonsDown(&ctx.pad);
        if (down & HidNpadButton_Plus) { ctx.exit_requested = true; return PreflightDecision::Cancel; }
        if (down & HidNpadButton_B) return PreflightDecision::Cancel;
        if (requires_force && (down & HidNpadButton_A)) return PreflightDecision::Skip;
        if (requires_force && (down & HidNpadButton_X)) return PreflightDecision::ForceInstall;
        if (!requires_force && (down & HidNpadButton_A)) return PreflightDecision::Install;
        svcSleepThread(10'000'000);
    }
    return PreflightDecision::Cancel;
}
std::string translate_install_stage(const std::string& stage) {
    const std::string lower = lower_copy(stage);
    if (lower.find("verify") != std::string::npos || lower.find("valid") != std::string::npos) return "正在校验安装内容";
    if (lower.find("ticket") != std::string::npos) return "正在写入票据信息";
    if (lower.find("content") != std::string::npos || lower.find("nca") != std::string::npos) return "正在写入游戏内容";
    if (lower.find("commit") != std::string::npos || lower.find("final") != std::string::npos) return "正在完成安装";
    if (lower.find("prepare") != std::string::npos || lower.find("open") != std::string::npos) return "正在准备安装";
    return "正在安装";
}

astranas::user_backend::InstallOptions make_install_options(
        AppConfig& config, ActionContext& ctx, const std::string& source_path,
        bool ask_confirmation, bool delete_after_success) {
    astranas::user_backend::InstallOptions options{};
    options.ignore_required_firmware = config.ignore_required_firmware;
    options.validate_nca = config.validate_nca;
    options.verify_nca_content_hash = config.verify_nca_content_hash;
    options.storage = config.install_to_nand ? astranas::user_backend::InstallStorage::NandUser
                                             : astranas::user_backend::InstallStorage::SdCard;
    options.preflight = [&](const astranas::user_backend::PackageInstallInfo& info) {
        return choose_preflight(ctx, source_path, info, ask_confirmation, delete_after_success);
    };
    options.invalid_nca = [&](const std::string& content_id) {
        const std::string detail =
            "检测到 NCA 签名无效。\n\n"
            "Content ID：" + content_id +
            "\n\n继续安装会永久关闭“NCA 校验”。关闭后仍会检查 NCA 结构和声明大小，"
            "但不再阻止头签名异常。仅在你确认安装包来源可信时继续。";
        if (!astranas::ui::confirm_action(ctx.gui, ctx.pad, ctx.exit_requested,
                "检测到无效 NCA 签名", detail, "理解风险并永久关闭", "取消安装"))
            return false;
        config.validate_nca = false;
        std::string save_error;
        if (!save_config(kConfigPath, config, save_error))
            append_debug_log("永久关闭 NCA 校验失败", save_error);
        else
            append_debug_log("NCA 校验", "用户确认风险后已永久关闭 NCA 头签名校验");
        return true;
    };
    return options;
}

astranas::install_performance::Snapshot log_install_performance(
        const std::chrono::steady_clock::time_point& started) {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto stats = astranas::install_performance::snapshot();
    append_debug_log("安装性能", astranas::install_performance::summary(stats, seconds));
    return stats;
}

double live_mib_per_second(std::uint64_t bytes, std::uint64_t ns) {
    if (bytes == 0 || ns == 0) return 0.0;
    return (static_cast<double>(bytes) / (1024.0 * 1024.0)) /
           (static_cast<double>(ns) / 1'000'000'000.0);
}

std::string live_install_speed_text(const astranas::install_performance::Snapshot& stats,
                                    bool network_source) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    if (network_source)
        out << "下载：" << live_mib_per_second(stats.source_bytes, stats.source_read_ns) << " MiB/s · ";
    out << "安装写入：" << live_mib_per_second(stats.ncm_bytes, stats.ncm_write_ns) << " MiB/s";
    if (network_source && (stats.source_wait_ns || stats.source_backpressure_ns)) {
        out << "\n等网：" << (stats.source_wait_ns / 1'000'000ull) << " ms"
            << " · 网络回压：" << (stats.source_backpressure_ns / 1'000'000ull) << " ms";
    }
    return out.str();
}
} // namespace

InstallResult install_from_path(const std::string& source_path, AppConfig& config,
                                ActionContext& ctx, std::string& status,
                                bool delete_after_success, bool ask_confirmation,
                                bool notify_on_success) {
    const auto kind = detect_install_candidate(source_path);
    if (kind == InstallCandidateKind::None) { status = "所选项目不是可安装文件"; return InstallResult::Unsupported; }
    PackageInspection inspection;
    std::string inspect_error;
    inspect_package_file(source_path, inspection, inspect_error);
    if (!inspect_error.empty()) append_debug_log("安装包检查", inspect_error);
    const std::string target = config.install_to_nand ? "主机存储（NAND）" : "SD 卡";
    const std::string detail = basename_of(source_path) + "\n格式：" + container_label(inspection.kind) +
                               "\n目标：" + target + "\n安装成功后删除源文件：" + (delete_after_success ? "是" : "否");
    if (kind == InstallCandidateKind::HomebrewNro && ask_confirmation &&
        !astranas::ui::confirm_action(ctx.gui, ctx.pad, ctx.exit_requested, "安装 NRO 自制程序", detail, "开始安装", "取消")) {
        status = ctx.exit_requested ? "正在退出" : "安装已取消，源文件已保留";
        return InstallResult::Cancelled;
    }
    std::string installed_path, error;
    auto options = make_install_options(config, ctx, source_path, ask_confirmation, delete_after_success);
    astranas::install_performance::reset();
    const auto perf_started = std::chrono::steady_clock::now();
    const auto result = install_staged_file(source_path, installed_path, error,
        [&](const std::string& stage, std::uint64_t done, std::uint64_t total) {
            bool cancelled = false;
            if (poll_cancel(ctx, cancelled)) return false;
            const auto live = astranas::install_performance::snapshot();
            astranas::ui::draw_progress_page(ctx.gui, translate_install_stage(stage), basename_of(source_path),
                                             "安装目标：" + target + "\n" + live_install_speed_text(live, false),
                                             done, total, 0.0);
            return true;
        }, options);
    const auto perf = log_install_performance(perf_started);
    const std::string perf_text = astranas::install_performance::compact_summary(perf);
    if (result == InstallResult::Skipped) { status = "已跳过相同版本或降级安装包，源文件已保留"; return result; }
    if (result != InstallResult::Success) {
        if (result == InstallResult::Unsupported) status = friendly_error("安装后端不可用", error);
        else if (result == InstallResult::Cancelled) status = ctx.exit_requested ? "正在退出" : "安装已取消，源文件已保留";
        else status = friendly_error("安装失败", error);
        return result;
    }
    if (notify_on_success) astranas::completion_sound::play_success();
    if (!delete_after_success) {
        status = "安装成功，源文件按设置保留" + (perf_text.empty() ? std::string{} : " · " + perf_text);
        if (!error.empty()) append_debug_log("安装警告", error);
        return InstallResult::Success;
    }
    std::string cleanup_error;
    if (!remove_installed_package_source(source_path, cleanup_error)) {
        status = friendly_error("安装成功，但源文件清理失败", cleanup_error);
        if (!error.empty()) append_debug_log("安装警告", error);
        return InstallResult::InstalledCleanupFailed;
    }
    status = "安装成功，源文件已删除" + (perf_text.empty() ? std::string{} : " · " + perf_text);
    if (!error.empty()) append_debug_log("安装警告", error);
    return InstallResult::Success;
}
InstallResult install_from_cache(const std::string& cached_path, AppConfig& config,
                                 ActionContext& ctx, std::string& status, bool ask_confirmation,
                                 bool notify_on_success) {
    return install_from_path(cached_path, config, ctx, status, true,
                             ask_confirmation, notify_on_success);
}

InstallResult install_remote_entry(RemoteClient& remote, const RemoteDirEntry& entry,
                                   AppConfig& config, ActionContext& ctx,
                                   std::string& status, bool ask_confirmation,
                                   bool notify_on_success) {
    const auto fallback_to_cache = [&]() -> InstallResult {
        std::string cached;
        if (!stage_remote_candidate(remote, entry, config, ctx, status, cached))
            return status.find("取消") != std::string::npos ? InstallResult::Cancelled : InstallResult::Failed;
        return install_from_cache(cached, config, ctx, status, ask_confirmation, notify_on_success);
    };

    const auto kind = detect_package_container(entry.path);
    const bool direct_kind = kind == PackageContainerKind::Nsp || kind == PackageContainerKind::Nsz ||
                             kind == PackageContainerKind::Xci || kind == PackageContainerKind::Xcz;
    // NRO is not an NCM title stream. Even when network direct install is enabled,
    // stage the selected NRO internally and then install it into sdmc:/switch. The
    // cache page itself remains hidden in direct-install mode.
    if (!config.network_direct_install || kind == PackageContainerKind::HomebrewNro)
        return fallback_to_cache();
    if (entry.is_dir) {
        status = "网络直装不可用：所选项目是目录，不是安装包文件";
        return InstallResult::Unsupported;
    }
    if (!direct_kind) {
        status = "网络直装不可用：仅支持 NSP/NSZ/XCI/XCZ";
        return InstallResult::Unsupported;
    }
    if (astranas::title_backend::is_split_package_first_part(entry.path)) {
        status = "网络直装不可用：远程分卷安装包暂不支持直装；如需安装，请关闭“网络直装”后使用缓存模式";
        return InstallResult::Unsupported;
    }

    astranas::network::ScopedCpuBoost network_boost;
    astranas::remote::RemotePackageSource source(remote, entry);
    std::string direct_error;
    if (!source.open(entry.path, direct_error)) {
        append_debug_log("网络直装不可用", direct_error);
        status = friendly_error("网络直装不可用", direct_error);
        return InstallResult::Unsupported;
    }

    const std::string target = config.install_to_nand ? "主机存储（NAND）" : "SD 卡";
    auto options = make_install_options(config, ctx, entry.path, ask_confirmation, false);
    astranas::install_performance::reset();
    const auto perf_started = std::chrono::steady_clock::now();
    std::string installed_path;
    std::string error;
    const auto result = install_package_source(entry.path, kind, source, installed_path, error,
        [&](const std::string& stage, std::uint64_t done, std::uint64_t total) {
            bool cancelled = false;
            if (poll_cancel(ctx, cancelled)) return false;
            const auto live = astranas::install_performance::snapshot();
            astranas::ui::draw_progress_page(ctx.gui, translate_install_stage(stage), entry.name,
                                             "网络直装 · " + remote.protocol_name() + " → " + target +
                                             "\n" + live_install_speed_text(live, true),
                                             done, total, 0.0);
            return true;
        }, options);
    const auto perf = log_install_performance(perf_started);
    const std::string perf_text = astranas::install_performance::compact_summary(perf);
    std::string finish_error;
    const bool source_consistent = result == InstallResult::Cancelled || ctx.exit_requested
        ? true : source.finish(finish_error);
    if (!source_consistent) {
        append_debug_log("网络直装源文件最终一致性检查",
                         "path=" + entry.path + "\n" + finish_error);
        if (!error.empty()) error += "; ";
        error += "NAS 文件收尾一致性检查失败: " + finish_error;
    }

    if (result == InstallResult::Success) {
        status = "网络直装成功，NAS 源文件已保留" +
                 (perf_text.empty() ? std::string{} : " · " + perf_text);
        if (!source_consistent) status += "；NAS 文件收尾检查失败，详情已写入日志";
        if (!error.empty()) append_debug_log("安装警告", error);
        if (notify_on_success) astranas::completion_sound::play_success();
        return result;
    }
    if (result == InstallResult::Skipped) {
        status = "已跳过相同版本或降级安装包，NAS 源文件已保留";
        if (!source_consistent) status += "；NAS 文件收尾检查失败，详情已写入日志";
        return result;
    }
    if (result == InstallResult::Unsupported) status = friendly_error("网络直装后端不可用", error);
    else if (result == InstallResult::Cancelled) status = ctx.exit_requested ? "正在退出" : "网络直装已取消";
    else status = friendly_error("网络直装失败", error);
    return result;
}
bool install_local_batch(const std::vector<std::string>& candidates, AppConfig& config,
                         ActionContext& ctx, std::string& status, bool recursive) {
    if (candidates.empty()) { status = "当前目录没有可安装文件"; return false; }
    const std::string target = config.install_to_nand ? "主机存储（NAND）" : "SD 卡";
    const std::string detail = "共 " + std::to_string(candidates.size()) + " 个安装包" + (recursive ? "（包含子目录）" : "") +
        "\n执行顺序：严格按选择顺序 #1 → #2 → … 串行安装" +
        "\n安装目标：" + target + "\n相同版本和降级内容默认跳过\n安装成功后的源文件：" +
        (config.delete_source_after_install ? "删除" : "保留");
    if (!astranas::ui::confirm_action(ctx.gui, ctx.pad, ctx.exit_requested, "批量安装确认", detail, "开始批量安装", "取消")) {
        status = ctx.exit_requested ? "正在退出" : "批量安装已取消"; return false;
    }
    std::size_t installed = 0, cleanup_failed = 0, skipped = 0, failed = 0;
    std::string last_failure;
    for (const auto& path : candidates) {
        const auto result = install_from_path(path, config, ctx, status,
                                              config.delete_source_after_install, false, false);
        if (result == InstallResult::Success) ++installed;
        else if (result == InstallResult::InstalledCleanupFailed) { ++installed; ++cleanup_failed; }
        else if (result == InstallResult::Skipped) ++skipped;
        else { ++failed; last_failure = status; if (result == InstallResult::Cancelled || ctx.exit_requested) break; }
    }
    status = "批量安装完成：成功 " + std::to_string(installed) + "，跳过 " + std::to_string(skipped) + "，失败 " + std::to_string(failed);
    if (cleanup_failed) status += "，其中 " + std::to_string(cleanup_failed) + " 项源文件清理失败";
    if (!last_failure.empty()) append_debug_log("批量安装最后一个失败项目", last_failure);
    if (failed == 0 && (installed + skipped) > 0 && !ctx.exit_requested)
        astranas::completion_sound::play_success();
    return failed == 0;
}

bool prepare_remote_install_queue(RemoteClient& remote,
                                  std::vector<RemoteInstallQueueEntry>& install_queue,
                                  AppConfig& config, ActionContext& ctx,
                                  std::string& status) {
    using astranas::user_backend::InstallOptions;
    using astranas::user_backend::PackageInstallInfo;
    using astranas::user_backend::PreflightDecision;

    if (install_queue.empty()) { status = "安装队列为空"; return false; }

    const bool direct_install = config.network_direct_install;
    if (direct_install) {
        for (auto& queued : install_queue) {
            if (queued.info_state == RemoteInstallInfoState::Unavailable) {
                queued.info_state = RemoteInstallInfoState::Pending;
                queued.install_info = {};
            }
        }
    } else if (std::any_of(install_queue.begin(), install_queue.end(), [](const auto& queued) {
                   return queued.info_state == RemoteInstallInfoState::Unavailable;
               })) {
        status = "部分安装包无法预解析 Title ID / 内容类型；已保留当前顺序，请在顺序页确认本体位于更新和 DLC 前。";
        return true;
    }

    bool newly_prepared = false;
    std::string unavailable_name;
    std::string unavailable_error;
    for (std::size_t i = 0; i < install_queue.size(); ++i) {
        auto& queued = install_queue[i];
        if (queued.info_state != RemoteInstallInfoState::Pending) continue;

        const auto kind = detect_package_container(queued.entry.path);
        if (kind == PackageContainerKind::HomebrewNro) {
            queued.info_state = RemoteInstallInfoState::NotRequired;
            newly_prepared = true;
            continue;
        }
        if (queued.entry.is_dir ||
            (kind != PackageContainerKind::Nsp && kind != PackageContainerKind::Nsz &&
             kind != PackageContainerKind::Xci && kind != PackageContainerKind::Xcz) ||
            astranas::title_backend::is_split_package_first_part(queued.entry.path)) {
            queued.info_state = RemoteInstallInfoState::Unavailable;
            unavailable_name = queued.entry.name;
            unavailable_error = "该远程项目不能通过单个 PackageSource 预解析";
        } else {
            astranas::remote::RemotePackageSource source(remote, queued.entry);
            std::string error;
            PackageInstallInfo prepared_info;
            bool captured_info = false;
            if (source.open(queued.entry.path, error)) {
                InstallOptions options{};
                options.ignore_required_firmware = config.ignore_required_firmware;
                options.validate_nca = config.validate_nca;
                options.verify_nca_content_hash = config.verify_nca_content_hash;
                options.storage = config.install_to_nand
                    ? astranas::user_backend::InstallStorage::NandUser
                    : astranas::user_backend::InstallStorage::SdCard;
                options.preflight = [&](const PackageInstallInfo& info) {
                    prepared_info = info;
                    captured_info = true;
                    // Prepare reads CNMT and installed versions. Skip here to roll
                    // back the temporary CNMT staging without writing game content.
                    return PreflightDecision::Skip;
                };

                std::string installed_path;
                const auto progress = [&](const std::string& stage, std::uint64_t done,
                                          std::uint64_t total) {
                    bool cancelled = false;
                    if (poll_cancel(ctx, cancelled)) return false;
                    const std::string detail = "预解析安装信息并生成推荐顺序 · " +
                        std::to_string(i + 1) + " / " + std::to_string(install_queue.size());
                    astranas::ui::draw_progress_page(ctx.gui, translate_install_stage(stage),
                        queued.entry.name, detail, done, total, 0.0);
                    return true;
                };
                const auto result = install_package_source(queued.entry.path, kind, source,
                    installed_path, error, progress, options);

                std::string finish_error;
                const bool source_finished = !source.prepared() || source.finish(finish_error);
                if (!source_finished) {
                    if (!error.empty()) error += "; ";
                    error += finish_error;
                }
                if (result == InstallResult::Cancelled || ctx.exit_requested) {
                    status = ctx.exit_requested ? "正在退出" : "安装顺序预解析已取消";
                    return false;
                }
                if (source_finished && result == InstallResult::Skipped && captured_info &&
                    !prepared_info.contents.empty()) {
                    queued.install_info = std::move(prepared_info);
                    queued.info_state = RemoteInstallInfoState::Available;
                    newly_prepared = true;
                    continue;
                }
                if (error.empty()) error = "无法从安装后端取得 Title ID / 内容类型";
                queued.info_state = RemoteInstallInfoState::Unavailable;
                unavailable_name = queued.entry.name;
                unavailable_error = error;
            } else {
                queued.info_state = RemoteInstallInfoState::Unavailable;
                unavailable_name = queued.entry.name;
                unavailable_error = error.empty() ? "远程安装包读取准备失败" : error;
            }
        }

        if (queued.info_state == RemoteInstallInfoState::Unavailable) {
            queued.install_info = {};
            if (direct_install) {
                status = friendly_error("无法预解析直装队列顺序",
                    unavailable_name + "：" + unavailable_error);
                return false;
            }
            status = "无法解析 " + unavailable_name + " 的 Title ID / 内容类型；缓存模式保留当前顺序。";
            break;
        }
    }

    const bool unresolved = std::any_of(install_queue.begin(), install_queue.end(), [](const auto& queued) {
        return queued.info_state == RemoteInstallInfoState::Pending ||
               queued.info_state == RemoteInstallInfoState::Unavailable;
    });
    if (unresolved) {
        if (!direct_install && status.empty())
            status = "部分安装包未能预解析 Title ID / 内容类型；已保留当前顺序，请手动确认依赖顺序。";
        return true;
    }

    const bool has_package_info = std::any_of(install_queue.begin(), install_queue.end(), [](const auto& queued) {
        return queued.info_state == RemoteInstallInfoState::Available;
    });
    if (newly_prepared && has_package_info) {
        std::stable_sort(install_queue.begin(), install_queue.end(), [](const auto& lhs, const auto& rhs) {
            return astranas::app::batch_install_order_less(lhs.install_info, rhs.install_info);
        });
        status = "已预解析批量安装信息；默认按 Title ID 和内容类型排列，可在顺序页手动调整。";
    } else if (newly_prepared && !has_package_info) {
        status = "队列项目不含可排序的游戏包信息；按当前顺序执行。";
    }
    return true;
}

bool run_install_queue(RemoteClient& remote, std::vector<RemoteInstallQueueEntry>& install_queue,
                       AppConfig& config, ActionContext& ctx, std::string& status) {
    if (install_queue.empty()) { status = "安装队列为空"; return false; }
    const bool unresolved_order_info = std::any_of(install_queue.begin(), install_queue.end(), [](const auto& queued) {
        return queued.info_state == RemoteInstallInfoState::Pending ||
               queued.info_state == RemoteInstallInfoState::Unavailable;
    });
    if (!astranas::ui::confirm_action(ctx.gui, ctx.pad, ctx.exit_requested, "执行安装队列",
                        "队列中共有 " + std::to_string(install_queue.size()) + " 个项目。"
                        "\n执行顺序：严格按当前列表 #1 → #2 → … 串行安装。"
                        "\nTitle ID 与内容类型用于生成默认顺序；顺序页中的手动调整优先。" +
                        (unresolved_order_info
                            ? "\n部分安装包无法预解析；本批保持当前顺序，请确认基础内容先于更新和 DLC。"
                            : std::string{}) +
                        "\n网络直装开启时只执行直装；不满足条件会明确报错，不会自动切换缓存。",
                        "开始执行", "取消")) {
        status = ctx.exit_requested ? "正在退出" : "安装队列已取消"; return false;
    }
    std::vector<RemoteInstallQueueEntry> remaining;
    std::size_t installed = 0, cleanup_failed = 0, skipped = 0, unsupported = 0, failed = 0;
    std::string last_failure;
    for (std::size_t i = 0; i < install_queue.size(); ++i) {
        const auto& entry = install_queue[i].entry;
        const auto result = install_remote_entry(remote, entry, config, ctx, status, false, false);
        if (result == InstallResult::Success) ++installed;
        else if (result == InstallResult::InstalledCleanupFailed) { ++installed; ++cleanup_failed; }
        else if (result == InstallResult::Skipped) ++skipped;
        else if (result == InstallResult::Unsupported) { ++unsupported; ++failed; last_failure = status; remaining.push_back(install_queue[i]); }
        else {
            ++failed; last_failure = status; remaining.push_back(install_queue[i]);
            if (result == InstallResult::Cancelled || ctx.exit_requested) {
                remaining.insert(remaining.end(), install_queue.begin() + static_cast<std::ptrdiff_t>(i + 1), install_queue.end()); break;
            }
        }
    }
    install_queue.swap(remaining);
    status = "队列执行完成：安装 " + std::to_string(installed) + "，跳过 " + std::to_string(skipped) +
             "，直装不支持 " + std::to_string(unsupported) + "，失败 " + std::to_string(failed) +
             "，剩余 " + std::to_string(install_queue.size());
    if (cleanup_failed) status += "，源文件清理失败 " + std::to_string(cleanup_failed) + " 项";
    if (!last_failure.empty()) status += "\n最后原因：" + last_failure;
    if (failed == 0 && (installed + skipped) > 0 && !ctx.exit_requested)
        astranas::completion_sound::play_success();
    return failed == 0;
}

} // namespace astranas::app
#endif
