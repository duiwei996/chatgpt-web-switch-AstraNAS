// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime_internal.hpp"
#ifdef __SWITCH__
#include "../atmo_xl/include/nx/ipc/tin_ipc.h"
#endif
#ifdef __SWITCH__
#include "runtime_helpers.hpp"
#include "actions.hpp"
#include "constants.hpp"
#include "util.hpp"
#include "../config.hpp"
#include "../download_cache.hpp"
#include "../package_inspect.hpp"
#include "../installed/catalog.hpp"
#include "../local_fs.hpp"
#include "../log_upload.hpp"
#include "../network_runtime.hpp"
#include "../remote/remote_client.hpp"
#include "../ui/dialogs.hpp"
#include "../ui/gui.hpp"
#include "../ui/input.hpp"
#include "../ui/pages.hpp"
#include "../usb_storage.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <switch.h>

namespace astranas::app {
using namespace runtime_detail;

int Runtime::execute() {
    initialize_input();
    if (!initialize_gui()) return 1;
    std::string wlan_error;
    if (!astranas::network::set_wireless_priority(true, wlan_error))
        append_debug_log("WLAN 优先模式", wlan_error);
    load_initial_config();
    log_scan_offset_ = local_file_size(kLogPath);
    loop();
    shutdown();
    return 0;
}

void Runtime::initialize_input() {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad_);
    input_.initialize_touch();
}

bool Runtime::initialize_gui() {
    std::string error;
    return gui_.initialize(error);
}

void Runtime::draw_current() {
    const bool show_cache = !config_.network_direct_install;
    if (!show_cache && tab_ == Tab::Cache) tab_ = Tab::Local;
    const std::string protocol = remote_ ? remote_->protocol_name() : "离线";
    if (tab_ == Tab::Installed && installed_stage_ > 0) load_visible_icons();
    switch (tab_) {
        case Tab::Remote: astranas::ui::draw_remote(gui_, remote_entries_, remote_sel_, remote_dir_, install_queue_, protocol, status_, show_cache); break;
        case Tab::Local: astranas::ui::draw_local(gui_, local_entries_, local_sel_, local_dir_, local_selection_, local_location_label(local_root_, usb_ready_), protocol, status_, show_cache); break;
        case Tab::Cache: astranas::ui::draw_cache(gui_, cache_entries_, cache_sel_, protocol, status_, show_cache); break;
        case Tab::Installed: astranas::ui::draw_installed(gui_, installed_entries_, installed_sel_, installed_icons_, protocol, status_, show_cache); break;
        case Tab::Settings: astranas::ui::draw_settings(gui_, config_, settings_sel_, protocol, status_, last_benchmark_, show_cache); break;
    }
}

bool Runtime::run_deferred_page_load() {
    if (tab_ == Tab::Remote && !remote_attempted_) {
        remote_attempted_ = true;
        if (connect_remote()) refresh_remote();
        dirty_ = true;
        return true;
    }
    if (tab_ == Tab::Local && !local_loaded_) {
        refresh_local();
        status_ = "本机文件：按 A 打开文件操作菜单";
        dirty_ = true;
        return true;
    }
    if (tab_ == Tab::Cache && !config_.network_direct_install && !cache_loaded_) {
        refresh_cache();
        dirty_ = true;
        return true;
    }
    if (tab_ == Tab::Installed && installed_stage_ == 0) {
        load_installed_cache();
        dirty_ = true;
        return true;
    }
    if (tab_ == Tab::Installed && installed_stage_ == 1) {
        refresh_installed_live();
        dirty_ = true;
        return true;
    }
    return false;
}

void Runtime::set_tab(Tab next) {
    if (config_.network_direct_install && next == Tab::Cache) next = Tab::Local;
    if (tab_ == next) return;
    tab_ = next;
    dirty_ = true;
}

bool Runtime::handle_tab_touch(const InputFrame& frame) {
    if (!frame.touch_tap || frame.touch_y < 80 || frame.touch_y >= 124) return false;
    const bool show_cache = !config_.network_direct_install;
    const Tab with_cache[] = {Tab::Remote, Tab::Local, Tab::Cache, Tab::Installed, Tab::Settings};
    const Tab direct[] = {Tab::Remote, Tab::Local, Tab::Installed, Tab::Settings};
    const Tab* tabs = show_cache ? with_cache : direct;
    const int count = show_cache ? 5 : 4;
    const int gap = 10;
    const int width = (1200 - gap * (count - 1)) / count;
    for (int i = 0; i < count; ++i) {
        const int x = 40 + i * (width + gap);
        if (frame.touch_x >= x && frame.touch_x < x + width) {
            set_tab(tabs[i]);
            return true;
        }
    }
    return false;
}

void Runtime::maybe_save_install_log() {
    const std::uint64_t current_size = local_file_size(kLogPath);
    if (current_size < log_scan_offset_) log_scan_offset_ = 0;
    if (current_size == log_scan_offset_) return;

    std::ifstream in(kLogPath, std::ios::binary);
    if (!in) {
        log_scan_offset_ = current_size;
        return;
    }
    in.seekg(static_cast<std::streamoff>(log_scan_offset_), std::ios::beg);
    std::string appended((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    log_scan_offset_ = current_size;
    if (appended.find("安装性能") == std::string::npos || exit_requested_) return;

    constexpr const char* kInstallReportPath = "sdmc:/switch/AstraNAS/install-latest.log";
    std::ostringstream report;
    report << "AstraNAS 安装诊断日志\n"
           << "版本: " << kVersion << "\n"
           << "协议: " << config_.protocol << "\n"
           << "网络: " << astranas::network::network_link_summary(astranas::network::query_network_link()) << "\n"
           << "WLAN Priority: " << (astranas::network::wireless_priority_optimized() ? "OptimizedForWlan" : "Default") << "\n"
           << "上传策略: 仅手动上传\n"
           << "\n--- 本次安装日志 ---\n"
           << appended;

    std::string error;
    if (!astranas::log_upload::write_text_file(kInstallReportPath, report.str(), error)) {
        append_debug_log("安装诊断日志生成失败", error);
        status_ += " · 日志生成失败";
        dirty_ = true;
        return;
    }

    status_ += " · 安装日志已保存本地；可在设置中手动上传";
    dirty_ = true;
}

void Runtime::loop() {
    ActionContext action_ctx{gui_, pad_, exit_requested_};
    while (appletMainLoop() && !exit_requested_) {
        if (dirty_) {
            draw_current();
            dirty_ = false;
            if (run_deferred_page_load()) continue;
        }
        const InputFrame frame = input_.update(pad_);
        if (frame.down & HidNpadButton_Plus) { exit_requested_ = true; continue; }
        if (frame.down & HidNpadButton_Minus) {
            ensure_nifm();
            astranas::ui::show_wifi_notice(gui_, pad_, exit_requested_, false);
            dirty_ = true;
            continue;
        }
        if (handle_tab_touch(frame)) continue;
        if (frame.down & HidNpadButton_L) { set_tab(astranas::ui::cycle_tab(tab_, -1, !config_.network_direct_install)); continue; }
        if (frame.down & HidNpadButton_R) { set_tab(astranas::ui::cycle_tab(tab_, +1, !config_.network_direct_install)); continue; }


        switch (tab_) {
            case Tab::Remote: handle_remote(frame, action_ctx); break;
            case Tab::Local: handle_local(frame, action_ctx); break;
            case Tab::Cache:
                if (config_.network_direct_install) set_tab(Tab::Local);
                else handle_cache(frame, action_ctx);
                break;
            case Tab::Installed: handle_installed(frame); break;
            case Tab::Settings: handle_settings(frame); break;
        }
        maybe_save_install_log();
        if (frame.down || frame.nav || frame.touch_tap || frame.touch_scroll_rows) dirty_ = true;
        svcSleepThread(8'000'000);
    }
}

void Runtime::shutdown() {
    remote_.reset();
    persist_local_state();
    if (usb_ready_) astranas::usb_storage::finalize();
    if (ns_ready_) nsextExit();
    if (nifm_ready_) nifmExit();
    if (curl_ready_) curl_global_cleanup();
    if (socket_ready_) socketExit();
    gui_.shutdown();
}

int run() { Runtime runtime; return runtime.execute(); }

} // namespace astranas::app
#else
#include <cstdio>
namespace astranas::app { int run() { std::fprintf(stderr, "AstraNAS 主程序仅用于 Nintendo Switch。\n"); return 1; } }
#endif
