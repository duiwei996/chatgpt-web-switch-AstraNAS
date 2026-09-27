// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime_internal.hpp"
#ifdef __SWITCH__
#include "runtime_helpers.hpp"
#include "actions.hpp"
#include "constants.hpp"
#include "util.hpp"
#include "../config.hpp"
#include "../download_cache.hpp"
#include "../installed/catalog.hpp"
#include "../atmo_xl/include/nx/ipc/tin_ipc.h"
#include "../local_fs.hpp"
#include "../network_runtime.hpp"
#include "../remote/remote_client.hpp"
#include "../ui/dialogs.hpp"
#include "../usb_storage.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include <switch.h>

namespace astranas::app {
using namespace runtime_detail;

void Runtime::load_initial_config() {
    local_mkdir_p(kBaseDir);
    local_mkdir_p(kCacheRoot);
    local_mkdir_p(kUiCacheDir);
    std::string error;
    if (!load_config(kConfigPath, config_, error)) {
        std::string create_error;
        write_example_config(kConfigPath, create_error);
        if (!load_config(kConfigPath, config_, error)) config_ = AppConfig{};
        first_run_ = true;
        status_ = create_error.empty() ? "首次使用：请先在“设置”中选择协议并填写 NAS 地址" : friendly_error("无法创建初始配置", create_error);
    }
    normalize_config_endpoint(config_);
    if (!local_path_is_within(kCacheRoot, config_.cache_dir)) config_.cache_dir = kCacheRoot;
    if (config_.local_root.empty()) config_.local_root = "sdmc:/";
    if (config_.local_dir.empty()) config_.local_dir = config_.local_root;
    local_mkdir_p(config_.cache_dir);
    local_root_ = config_.local_root;
    local_dir_ = config_.local_dir;
    remote_state_endpoint_ = remote_endpoint_identity(config_);
    remote_dir_ = config_.remote_dir.empty() ? remote_start_dir() : config_.remote_dir;
    validate_remote_state();
    tab_ = (first_run_ || config_.server.empty()) ? Tab::Settings : Tab::Local;
    if (!local_path_exists(kWifiNoticeSeenPath)) {
        ensure_nifm();
        astranas::ui::show_wifi_notice(gui_, pad_, exit_requested_, true);
    }
}

bool Runtime::ensure_nifm() {
    if (nifm_ready_) return true;
    if (nifm_attempted_) return false;
    nifm_attempted_ = true;
    nifm_ready_ = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    return nifm_ready_;
}
bool Runtime::ensure_network() {
    if (!socket_ready_) {
        std::string socket_error;
        if (!astranas::network::initialize_socket(astranas::network::SocketProfile::HighThroughput, socket_error)) {
            append_debug_log("高吞吐 socket 初始化", socket_error);
            if (!astranas::network::initialize_socket(astranas::network::SocketProfile::LibnxDefault, socket_error)) {
                status_ = friendly_error("网络模块初始化失败，请重新启动主机后再试", socket_error);
                return false;
            }
            append_debug_log("网络 socket", "高吞吐配置不可用，已回退 libnx 默认配置");
        }
        socket_ready_ = true;
    }
    if (!curl_ready_) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) { status_ = "网络传输模块初始化失败，请重新启动应用后再试"; return false; }
        curl_ready_ = true;
    }
    ensure_nifm();
    return true;
}
bool Runtime::ensure_ns() {
    if (ns_ready_) return true;
    if (ns_attempted_) return false;
    ns_attempted_ = true;
    ns_ready_ = R_SUCCEEDED(nsextInitialize());
    if (!ns_ready_) status_ = "系统应用管理服务不可用，暂时无法读取或管理已安装游戏";
    return ns_ready_;
}
bool Runtime::ensure_usb() {
    if (usb_ready_) return true;
    if (usb_attempted_) return false;
    usb_attempted_ = true;
    usb_ready_ = astranas::usb_storage::initialize();
    return usb_ready_;
}

std::string Runtime::remote_start_dir() const {
    if (config_.protocol == "smb" && config_.share.empty()) return "/";
    return config_.root.empty() ? "/" : config_.root;
}

bool Runtime::validate_remote_state() {
    const std::string bound = config_.protocol == "smb" && config_.share.empty() ? "/" : remote_start_dir();
    if (remote_dir_.empty() || !remote_path_within(bound, remote_dir_)) {
        remote_dir_ = remote_start_dir();
        config_.remote_dir = remote_dir_;
        return false;
    }
    return true;
}

void Runtime::persist_remote_state() {
    validate_remote_state();
    config_.remote_dir = remote_dir_;
    std::string error;
    if (!save_config(kConfigPath, config_, error)) append_debug_log("保存 NAS 浏览位置", error);
}

void Runtime::restore_remote_selection() {
    if (remote_restore_selection_path_.empty() || remote_entries_.empty()) return;
    const auto it = std::find_if(remote_entries_.begin(), remote_entries_.end(), [&](const auto& entry) {
        return entry.path == remote_restore_selection_path_;
    });
    if (it != remote_entries_.end()) remote_sel_ = static_cast<std::size_t>(std::distance(remote_entries_.begin(), it));
    remote_restore_selection_path_.clear();
}

void Runtime::validate_local_state() {
    if (local_root_.empty()) local_root_ = "sdmc:/";
    if (local_root_ == "sdmc:/") {
        if (!local_path_is_within(local_root_, local_dir_) || !local_path_exists(local_dir_)) local_dir_ = "sdmc:/";
        return;
    }
    ensure_usb();
    // A removable USB mount can appear after the app starts. Do not silently
    // replace its saved root with SD: that would hide completed downloads and
    // overwrite the only persisted path to them.
    if (!local_path_is_within(local_root_, local_dir_)) local_dir_ = local_root_;
}
void Runtime::persist_local_state() {
    config_.local_dir = local_dir_;
    config_.local_root = local_root_;
    std::string error;
    if (!save_config(kConfigPath, config_, error)) append_debug_log("保存本机浏览位置", error);
}
bool Runtime::refresh_local() {
    validate_local_state();
    std::string error;
    local_entries_.clear();
    const bool listed = list_local_dir(local_dir_, local_entries_, error);
    if (!listed) {
        if (local_location_label(local_root_, usb_ready_) == "设备已移除") {
            append_debug_log("本机存储设备当前不可用，已保留原保存位置",
                "local_root=" + local_root_ + "\nlocal_dir=" + local_dir_ + "\n" + error);
            status_ = "上次使用的本机存储当前未就绪，已保留路径；连接设备后刷新：" + local_dir_;
        } else {
            status_ = friendly_error("读取本机目录失败", error);
        }
    } else {
        local_entries_.erase(std::remove_if(local_entries_.begin(), local_entries_.end(), [](const auto& entry) { return is_transfer_sidecar_name(entry.name); }), local_entries_.end());
        // Directory enumeration can omit a valid UTF-8 name on a later refresh.
        // Keep every completed download reachable across folder navigation and app restarts.
        for (const auto& path : config_.local_download_paths) {
            if (local_parent_directory(path) != local_dir_ ||
                !local_path_is_within(local_root_, path) ||
                is_transfer_sidecar_name(basename_of(path)))
                continue;
            const auto listedPath = std::find_if(local_entries_.begin(), local_entries_.end(),
                [&](const LocalEntry& entry) { return entry.path == path; });
            if (listedPath != local_entries_.end()) continue;
            if (!local_path_exists(path)) {
                append_debug_log("已记录的本机下载路径暂不可回读",
                    "local_root=" + local_root_ + "\nlocal_dir=" + local_dir_ +
                    "\n" + local_path_diagnostic(path));
                continue;
            }
            LocalEntry fallback{};
            fallback.name = basename_of(path);
            fallback.path = path;
            fallback.size = local_file_size(path);
            fallback.is_dir = false;
            local_entries_.push_back(std::move(fallback));
            append_debug_log("目录枚举未返回已下载文件，按持久路径补入列表",
                "local_root=" + local_root_ + "\nlocal_dir=" + local_dir_ +
                "\n" + local_path_diagnostic(path));
        }
        const auto remembered = local_selected_by_dir_.find(local_dir_);
        std::string restore_path;
        bool restore_from_config = false;
        if (remembered != local_selected_by_dir_.end()) {
            restore_path = remembered->second;
        } else if (!config_.local_focus_path.empty() &&
                   local_path_is_within(local_root_, config_.local_focus_path) &&
                   config_.local_focus_path != local_dir_) {
            std::string candidate = config_.local_focus_path;
            std::string parent = local_parent_directory(candidate);
            while (!parent.empty() && parent != local_dir_ && parent != candidate &&
                   local_path_is_within(local_root_, parent)) {
                candidate = parent;
                parent = local_parent_directory(candidate);
            }
            if (parent == local_dir_) {
                restore_path = candidate;
                restore_from_config = true;
            }
        }
        if (!restore_path.empty()) {
            const std::string path = restore_path;
            auto found = std::find_if(local_entries_.begin(), local_entries_.end(),
                [&](const LocalEntry& entry) { return entry.path == path; });
            if (found == local_entries_.end() && (!restore_from_config || path == config_.local_focus_path) &&
                local_parent_directory(path) == local_dir_ &&
                local_path_is_within(local_root_, path) &&
                !is_transfer_sidecar_name(basename_of(path)) &&
                local_path_exists(path)) {
                LocalEntry fallback{};
                fallback.name = basename_of(path);
                fallback.path = path;
                fallback.size = local_file_size(path);
                fallback.is_dir = false;
                local_entries_.push_back(std::move(fallback));
                found = std::prev(local_entries_.end());
                append_debug_log("本机目录恢复选中项时以 stat 回读补项",
                    "local_root=" + local_root_ + "\nlocal_dir=" + local_dir_ +
                    "\n" + local_path_diagnostic(path));
            }
            if (found != local_entries_.end()) {
                local_sel_ = static_cast<std::size_t>(std::distance(local_entries_.begin(), found));
            } else {
                append_debug_log(restore_from_config ? "本机目录保存的文件定位路径回读失败" : "本机目录上次选中项回读失败",
                    "local_root=" + local_root_ + "\nlocal_dir=" + local_dir_ +
                    "\n" + local_path_diagnostic(path));
                if (!restore_from_config) local_selected_by_dir_.erase(remembered);
                else if (path == config_.local_focus_path) config_.local_focus_path.clear();
            }
        } else if (local_entries_.empty()) {
            local_sel_ = 0;
        } else if (local_sel_ >= local_entries_.size()) {
            local_sel_ = local_entries_.size() - 1;
        }
    }
    local_selection_.erase(std::remove_if(local_selection_.begin(), local_selection_.end(), [](const std::string& path) { return !local_path_exists(path); }), local_selection_.end());
    persist_local_state();
    local_loaded_ = true;
    return listed;
}
void Runtime::refresh_cache() {
    std::string error;
    cache_entries_.clear();
    if (!list_local_dir(config_.cache_dir, cache_entries_, error)) status_ = friendly_error("读取安装缓存失败", error);
    else cache_entries_.erase(std::remove_if(cache_entries_.begin(), cache_entries_.end(), [](const auto& entry) { return is_transfer_sidecar_name(entry.name); }), cache_entries_.end());
    cache_loaded_ = true;
}
void Runtime::load_installed_cache() {
    std::string error;
    installed_entries_.clear();
    if (!astranas::installed::load_cache(kUiCacheDir, installed_entries_, error)) append_debug_log("读取游戏缓存", error);
    status_ = installed_entries_.empty() ? "游戏缓存为空，正在读取系统应用记录..." : "已从缓存快速显示游戏列表，正在检查变化...";
    installed_stage_ = 1;
}
void Runtime::refresh_installed_live() {
    if (!ensure_ns()) { installed_stage_ = 2; return; }
    std::string error;
    if (!astranas::installed::refresh(kUiCacheDir, installed_entries_, error)) status_ = friendly_error("刷新已安装游戏失败", error);
    else status_ = "已更新 " + std::to_string(installed_entries_.size()) + " 个已安装游戏；未变化的图标已复用缓存";
    installed_icons_.clear();
    installed_stage_ = 2;
}
void Runtime::load_visible_icons() {
    if (installed_entries_.empty()) return;
    constexpr std::size_t rows = 7;
    const std::size_t begin = visible_begin(installed_sel_, installed_entries_.size(), rows);
    const std::size_t end = std::min(installed_entries_.size(), begin + rows);
    for (std::size_t i = begin; i < end; ++i) {
        const auto id = installed_entries_[i].application_id;
        if (installed_icons_.find(id) != installed_icons_.end()) continue;
        astranas::installed::IconBitmap bitmap;
        std::string error;
        if (astranas::installed::load_icon_rgb(kUiCacheDir, id, bitmap.rgb, bitmap.width, bitmap.height, error)) installed_icons_.emplace(id, std::move(bitmap));
    }
}

bool Runtime::connect_remote(bool test_only) {
    const std::string wanted_dir = remote_dir_;
    remote_.reset();
    remote_entries_.clear();
    normalize_config_endpoint(config_);
    if (config_.server.empty()) { status_ = "尚未设置 NAS 地址，请前往“设置”填写服务器地址"; return false; }
    if (!ensure_network()) return false;
    std::string error;
    remote_ = make_remote_client(config_, error);
    if (!remote_) { status_ = friendly_error("无法创建远程连接", error); return false; }
    if (!remote_->connect(config_, error)) { status_ = friendly_error(test_only ? "测试连接失败" : "连接 NAS 失败", error); remote_.reset(); return false; }
    remote_dir_ = wanted_dir;
    validate_remote_state();
    if (test_only) {
        std::vector<RemoteDirEntry> probe;
        if (!remote_->list_dir(remote_dir_, probe, error)) { status_ = friendly_error("测试连接失败", error); remote_.reset(); return false; }
        status_ = "连接测试成功：" + remote_->protocol_name() + "，可读取 " + std::to_string(probe.size()) + " 项";
        return true;
    }
    status_ = "已连接 NAS：" + remote_->protocol_name();
    return true;
}
void Runtime::refresh_remote() {
    remote_entries_.clear();
    if (!remote_) {
        if (config_.server.empty()) { status_ = "请先在“设置”填写 NAS 地址"; return; }
        if (!connect_remote()) return;
    }
    std::string error;
    if (!remote_->list_dir(remote_dir_, remote_entries_, error)) {
        append_debug_log("NAS 目录读取失败，尝试原路径重连", error);
        std::string reconnect_error;
        if (!remote_->connect(config_, reconnect_error)) {
            status_ = friendly_error("读取 NAS 目录失败", error + "; reconnect: " + reconnect_error);
            return;
        }
        error.clear();
        if (!remote_->list_dir(remote_dir_, remote_entries_, error)) {
            status_ = friendly_error("NAS 已重连，但原目录仍无法读取", error);
            return;
        }
        status_ = "NAS 已自动重连，并恢复原目录：" + remote_dir_;
        restore_remote_selection();
        persist_remote_state();
        return;
    }
    restore_remote_selection();
    persist_remote_state();
    status_ = "NAS 当前目录共有 " + std::to_string(remote_entries_.size()) + " 项";
}
void Runtime::disconnect_for_setting_change(const std::string& label) {
    remote_.reset();
    remote_entries_.clear();
    remote_attempted_ = false;
    const std::string endpoint = remote_endpoint_identity(config_);
    if (endpoint != remote_state_endpoint_) {
        remote_state_endpoint_ = endpoint;
        remote_dir_ = config_.remote_dir.empty() ? remote_start_dir() : config_.remote_dir;
        validate_remote_state();
        config_.remote_dir = remote_dir_;
    } else {
        validate_remote_state();
    }
    status_ = "已保存：" + label + "；NAS 将在进入文件页或测试连接时重新连接";
}
bool Runtime::save_config_now(const std::string& label, bool network_changed) {
    normalize_config_endpoint(config_);
    if (network_changed) {
        const std::string endpoint = remote_endpoint_identity(config_);
        if (endpoint != remote_state_endpoint_) {
            remote_state_endpoint_ = endpoint;
            remote_dir_ = config_.remote_dir.empty() ? remote_start_dir() : config_.remote_dir;
            validate_remote_state();
        } else {
            validate_remote_state();
        }
    }
    config_.local_dir = local_dir_;
    config_.local_root = local_root_;
    config_.remote_dir = remote_dir_;
    std::string error;
    if (!save_config(kConfigPath, config_, error)) { status_ = friendly_error("保存设置失败", error); return false; }
    if (network_changed) disconnect_for_setting_change(label);
    else status_ = "已保存：" + label;
    return true;
}

} // namespace astranas::app
#endif
