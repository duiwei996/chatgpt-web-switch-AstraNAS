// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime_internal.hpp"
#ifdef __SWITCH__
#include "runtime_helpers.hpp"
#include "actions.hpp"
#include "constants.hpp"
#include "util.hpp"
#include "../config.hpp"
#include "../installed/catalog.hpp"
#include "../installed/content_manager.hpp"
#include "../local_fs.hpp"
#include "../log_upload.hpp"
#include "../network_diagnostics.hpp"
#include "../network_runtime.hpp"
#include "../remote/remote_client.hpp"
#include "../ui/dialogs.hpp"
#include "../ui/gui.hpp"
#include "../ui/input.hpp"
#include "../ui/pages.hpp"
#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <switch.h>

namespace astranas::app {
using namespace runtime_detail;

void Runtime::handle_installed(const InputFrame& frame) {
    const std::size_t count = installed_entries_.size();
    if (frame.nav & HidNpadButton_Up) installed_sel_ = count ? (installed_sel_ == 0 ? count - 1 : installed_sel_ - 1) : 0;
    if (frame.nav & HidNpadButton_Down) installed_sel_ = count ? (installed_sel_ + 1) % count : 0;
    if (frame.touch_scroll_rows) clamp_scroll(installed_sel_, count, frame.touch_scroll_rows);

    bool touch_activate = false, touch_refresh = false;
    if (frame.touch_tap && frame.touch_y >= 282 && frame.touch_y < 618) {
        const std::size_t begin = visible_begin(installed_sel_, count, 7);
        const std::size_t row = static_cast<std::size_t>((frame.touch_y - 282) / 48);
        if (begin + row < count) { installed_sel_ = begin + row; touch_activate = true; }
    }
    if (frame.touch_tap && frame.touch_y >= 640) {
        if (frame.touch_x < 300) touch_activate = true;
        else if (frame.touch_x < 560) touch_refresh = true;
    }

    if ((frame.down & HidNpadButton_Y) || touch_refresh) {
        installed_stage_ = 1;
        refresh_installed_live();
        return;
    }
    if (!((frame.down & HidNpadButton_A) || touch_activate) || installed_sel_ >= installed_entries_.size()) return;
    if (!ensure_ns()) return;

    const auto entry = installed_entries_[installed_sel_];
    const std::string label = entry.name.empty() ? title_id_text(entry.application_id) : entry.name;
    std::vector<astranas::installed::ManagedContentMeta> dlcs;
    std::string dlc_error;
    const bool dlc_list_ok = astranas::installed::list_dlc(entry.application_id, dlcs, dlc_error);

    std::ostringstream detail;
    detail << label << "\nTitle ID：" << title_id_text(entry.application_id)
           << "\n升级包：" << (entry.patch_version ? ("v" + std::to_string(entry.patch_version)) : "未安装")
           << " · DLC：" << (dlc_list_ok ? std::to_string(dlcs.size()) : "状态未知");

    const int choice = astranas::ui::choose_action(
        gui_, pad_, input_, exit_requested_, "已安装游戏操作", detail.str(),
        {"卸载升级包", "卸载全部 DLC", "管理 DLC", "卸载全部游戏内容", "返回"});
    if (choice < 0 || choice == 4) return;

    auto finish_partial = [&](const std::string& action,
                              const astranas::installed::RemovalSummary& summary) {
        installed_stage_ = 1;
        refresh_installed_live();
        std::ostringstream out;
        out << action << "完成：删除 " << summary.meta_records << " 条内容记录";
        if (summary.content_files) out << "，清理 " << summary.content_files << " 个 orphan NCA";
        if (!summary.warning.empty()) out << "；" << summary.warning;
        status_ = out.str();
        if (installed_sel_ >= installed_entries_.size() && installed_sel_ > 0) --installed_sel_;
    };

    if (choice == 0) {
        if (!entry.patch_version) { status_ = "当前游戏没有已安装升级包"; return; }
        if (!astranas::ui::confirm_action(
                gui_, pad_, exit_requested_, "卸载升级包",
                label + "\n只删除 Patch/升级内容；保留游戏本体、DLC 和存档。",
                "确认卸载升级包", "取消")) {
            if (!exit_requested_) status_ = "卸载升级包已取消";
            return;
        }
        astranas::installed::RemovalSummary summary;
        std::string error;
        if (!astranas::installed::remove_update(entry.application_id, summary, error)) {
            status_ = friendly_error("卸载升级包失败", error);
            return;
        }
        finish_partial("升级包卸载", summary);
        return;
    }

    if (choice == 1) {
        if (!dlc_list_ok) { status_ = friendly_error("无法读取 DLC", dlc_error); return; }
        if (dlcs.empty()) { status_ = "当前游戏没有已安装 DLC"; return; }
        if (!astranas::ui::confirm_action(
                gui_, pad_, exit_requested_, "卸载全部 DLC",
                label + "\n将删除全部 " + std::to_string(dlcs.size()) +
                    " 个 DLC 及其 DataPatch；保留游戏本体、升级包和存档。",
                "确认卸载全部 DLC", "取消")) {
            if (!exit_requested_) status_ = "卸载全部 DLC 已取消";
            return;
        }
        astranas::installed::RemovalSummary summary;
        std::string error;
        if (!astranas::installed::remove_all_dlc(entry.application_id, summary, error)) {
            status_ = friendly_error("卸载全部 DLC 失败", error);
            return;
        }
        finish_partial("全部 DLC 卸载", summary);
        return;
    }

    if (choice == 2) {
        if (!dlc_list_ok) { status_ = friendly_error("无法读取 DLC", dlc_error); return; }
        if (dlcs.empty()) { status_ = "当前游戏没有已安装 DLC"; return; }
        std::vector<std::string> labels;
        labels.reserve(dlcs.size());
        for (const auto& dlc : dlcs) {
            labels.push_back("DLC " + title_id_text(dlc.id) + " · v" + std::to_string(dlc.version));
        }
        const int selected = astranas::ui::choose_action(
            gui_, pad_, input_, exit_requested_, "管理 DLC",
            label + "\n选择一个 DLC 单独卸载；↑↓ 可滚动全部项目。", labels);
        if (selected < 0 || static_cast<std::size_t>(selected) >= dlcs.size()) return;
        const auto dlc = dlcs[static_cast<std::size_t>(selected)];
        const std::string dlc_label = labels[static_cast<std::size_t>(selected)];
        if (!astranas::ui::confirm_action(
                gui_, pad_, exit_requested_, "卸载单个 DLC",
                dlc_label + "\n只删除该 DLC 及其关联 DataPatch；不删除本体、升级包或存档。",
                "确认卸载 DLC", "取消")) {
            if (!exit_requested_) status_ = "卸载 DLC 已取消";
            return;
        }
        astranas::installed::RemovalSummary summary;
        std::string error;
        if (!astranas::installed::remove_dlc(entry.application_id, dlc, summary, error)) {
            status_ = friendly_error("卸载 DLC 失败", error);
            return;
        }
        finish_partial("DLC 卸载", summary);
        return;
    }

    if (choice == 3) {
        if (!astranas::ui::confirm_action(
                gui_, pad_, exit_requested_, "卸载全部游戏内容",
                label + "\n" + title_id_text(entry.application_id) +
                    "\n将删除本体、升级包和 DLC；不主动删除游戏存档。",
                "确认卸载全部内容", "取消")) {
            if (!exit_requested_) status_ = "卸载全部游戏内容已取消";
            return;
        }
        const Result rc = nsDeleteApplicationEntity(entry.application_id);
        if (R_SUCCEEDED(rc)) {
            installed_stage_ = 1;
            refresh_installed_live();
            status_ = "已卸载全部游戏内容：" + label + "；未请求删除存档";
            if (installed_sel_ >= installed_entries_.size() && installed_sel_ > 0) --installed_sel_;
        } else {
            std::ostringstream out;
            out << "卸载全部游戏内容失败（系统错误代码 0x" << std::hex << rc << "）";
            status_ = out.str();
        }
    }
}

void Runtime::run_network_diagnostic() {
    if (!remote_ && !connect_remote(false)) return;
    if (!remote_) { status_ = "网络诊断失败：当前协议尚未连接"; return; }

    const std::string start_dir = remote_dir_.empty() ? remote_start_dir() : remote_dir_;
    astranas::ui::draw_simple_message(gui_, "网络诊断正在运行",
        "只从 NAS 读取到 RAM，不写 SD、不执行安装。\n\n协议：" + remote_->protocol_name() +
        "\n测试上限：64 MiB", "", "");

    astranas::network::DiagnosticResult result;
    std::string error;
    astranas::network::ScopedCpuBoost cpu_boost;
    if (!astranas::network::run_remote_diagnostic(*remote_, start_dir, kBenchmarkBytes, result, error)) {
        status_ = friendly_error("网络诊断失败", error);
        return;
    }
    last_benchmark_ = result.stats.mib_per_sec;
    std::string detail = astranas::network::format_diagnostic_summary(remote_->protocol_name(), result);
    const auto link = astranas::network::query_network_link();
    detail += "\n链路：" + astranas::network::network_link_summary(link);
    detail += std::string("\nWLAN Priority：") +
              (astranas::network::wireless_priority_optimized() ? "OptimizedForWlan" : "Default");
    detail += std::string("\nCPU Boost：") + (cpu_boost.enabled() ? "FastLoad" : "未启用");
    detail += "\n\n该测试不包含 SD 写入/NCM。可与独立 AstraNAS-NetDiag.nro 的结果对照。";
    astranas::ui::show_message_wait(gui_, pad_, exit_requested_, "网络诊断结果", detail);
    status_ = remote_->protocol_name() + " 纯网络测速：" + format_speed(result.stats.mib_per_sec);
}

void Runtime::handle_settings(const InputFrame& frame) {
    constexpr std::size_t count = astranas::ui::kSettingsCount;
    if (frame.nav & HidNpadButton_Up) settings_sel_ = settings_sel_ == 0 ? count - 1 : settings_sel_ - 1;
    if (frame.nav & HidNpadButton_Down) settings_sel_ = (settings_sel_ + 1) % count;
    if (frame.touch_scroll_rows) clamp_scroll(settings_sel_, count, frame.touch_scroll_rows);
    bool touch_activate = false;
    if (frame.touch_tap && frame.touch_y >= 252 && frame.touch_y < 604) {
        const std::size_t begin = visible_begin(settings_sel_, count, 8);
        const std::size_t row = static_cast<std::size_t>((frame.touch_y - 252) / 44);
        if (begin + row < count) { settings_sel_ = begin + row; touch_activate = true; }
    }
    if ((frame.down & HidNpadButton_B) && !touch_activate) { set_tab(Tab::Local); return; }
    if (!(frame.down & HidNpadButton_A) && !touch_activate) return;

    switch (settings_sel_) {
        case 0: {
            std::string selected;
            if (astranas::ui::choose_protocol(gui_, pad_, input_, exit_requested_, config_.protocol, selected) &&
                selected != config_.protocol) {
                config_.remote_dir = remote_dir_;
                set_active_protocol(config_, selected);
                remote_dir_ = config_.remote_dir.empty() ? remote_start_dir() : config_.remote_dir;
                save_config_now("连接协议", true);
            }
            break;
        }
        case 1: {
            SmbProfileConfig edited;
            if (astranas::ui::edit_smb_profile(gui_, pad_, input_, exit_requested_, config_.smb, edited)) {
                config_.smb = edited;
                if (config_.protocol == "smb") activate_remote_profile(config_);
                save_config_now("SMB 配置", config_.protocol == "smb");
            }
            break;
        }
        case 2: {
            WebDavProfileConfig edited;
            if (astranas::ui::edit_webdav_profile(gui_, pad_, input_, exit_requested_, config_.webdav, edited)) {
                config_.webdav = edited;
                if (config_.protocol == "webdav") activate_remote_profile(config_);
                save_config_now("WebDAV 配置", config_.protocol == "webdav");
            }
            break;
        }
        case 3: config_.download_retries = (config_.download_retries + 1) % 6; save_config_now("失败重试次数", false); break;
        case 4: config_.network_direct_install = !config_.network_direct_install; save_config_now("网络直装", false); break;
        case 5: config_.verify_sha256 = !config_.verify_sha256; save_config_now("下载后整包 SHA-256", false); break;
        case 6: config_.verify_nca_content_hash = !config_.verify_nca_content_hash; save_config_now("NCA 内容 SHA-256", false); break;
        case 7: config_.install_to_nand = !config_.install_to_nand; save_config_now("安装位置", false); break;
        case 8: config_.delete_source_after_install = !config_.delete_source_after_install; save_config_now("安装后删除源文件", false); break;
        case 9: config_.ignore_required_firmware = !config_.ignore_required_firmware; save_config_now("忽略固件要求", false); break;
        case 10: config_.validate_nca = !config_.validate_nca; save_config_now("NCA 头签名校验", false); break;
        case 11: {
            constexpr const char* kInstallReportPath = "sdmc:/switch/AstraNAS/install-latest.log";
            AppConfig active = config_;
            activate_remote_profile(active);
            normalize_config_endpoint(active);
            const std::string current = astranas::log_upload::configured_directory(
                active, astranas::log_upload::LogDestinationKind::Install);
            const std::string local_state = local_file_size(kInstallReportPath) > 0
                ? "本地已有最近一次安装日志。" : "本地暂无可上传的安装日志。";
            const int choice = astranas::ui::choose_action(
                gui_, pad_, input_, exit_requested_, "安装日志",
                local_state + "\n上传策略：仅手动上传。\n上传目录：" +
                    (current.empty() ? std::string("未设置（手动上传时可选择）") : current),
                {"上传最近安装日志", "选择 / 重新选择上传目录", "清空上传目录", "返回"});
            if (choice < 0 || choice == 3 || exit_requested_) break;
            if (choice == 2) {
                astranas::log_upload::clear_configured_directory(
                    astranas::log_upload::LogDestinationKind::Install);
                status_ = "安装日志上传目录已清空";
                break;
            }
            if (choice == 1) {
                if (!remote_ && !connect_remote(false)) break;
                if (!remote_) { status_ = "目录设置失败：当前协议尚未连接"; break; }
                std::string selected, error;
                if (astranas::log_upload::configure_directory(
                        *remote_, active, astranas::log_upload::LogDestinationKind::Install,
                        gui_, input_, pad_, exit_requested_, "选择安装日志上传目录", selected, error)) {
                    status_ = "安装日志上传目录已保存：" + selected;
                } else if (error == "cancelled") {
                    status_ = "未修改安装日志上传目录";
                } else {
                    status_ = friendly_error("目录设置失败", error);
                }
                break;
            }

            if (local_file_size(kInstallReportPath) == 0) {
                status_ = "暂无可上传的安装日志；完成一次安装后会先保存到本地";
                break;
            }
            if (!remote_ && !connect_remote(false)) break;
            if (!remote_) { status_ = "安装日志上传失败：当前协议尚未连接"; break; }
            std::string reconnect_error;
            if (!remote_->connect(active, reconnect_error)) {
                append_debug_log("安装日志手动上传重连失败", reconnect_error);
                remote_.reset();
                remote_attempted_ = true;
                if (!connect_remote(false)) break;
            }
            std::string uploaded_path, error;
            bool uploaded = false;
            {
                astranas::network::ScopedCpuBoost cpu_boost;
                uploaded = astranas::log_upload::upload_log(
                    *remote_, active, gui_, input_, pad_, exit_requested_,
                    kInstallReportPath, "AstraNAS-install", "选择安装日志保存目录",
                    uploaded_path, error);
            }
            if (uploaded) {
                append_debug_log("安装日志手动上传", uploaded_path);
                status_ = "安装日志已手动上传：" + uploaded_path;
            } else if (error == "cancelled") {
                status_ = "安装日志上传已取消；本地日志已保留";
            } else {
                status_ = friendly_error("安装日志上传失败，本地日志已保留", error);
            }
            break;
        }
        case 12: connect_remote(true); break;
        case 13: run_network_diagnostic(); break;
        case 14: ensure_nifm(); astranas::ui::show_wifi_notice(gui_, pad_, exit_requested_, false); break;
    }
}

} // namespace astranas::app
#endif