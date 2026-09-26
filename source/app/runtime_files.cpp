// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime_internal.hpp"
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
#include "../remote/remote_client.hpp"
#include "../ui/dialogs.hpp"
#include "../ui/gui.hpp"
#include "../ui/input.hpp"
#include "../ui/pages.hpp"
#include "../usb_storage.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <memory>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <switch.h>

namespace astranas::app {
using namespace runtime_detail;
namespace {
bool path_at_or_below(const std::string& candidate, const std::string& root) {
    if (candidate == root) return true;
    if (root.empty()) return false;
    std::string prefix = root;
    if (prefix.back() != '/') prefix.push_back('/');
    return candidate.rfind(prefix, 0) == 0;
}

std::string batch_label(const RemoteDirEntry& entry) { return entry.name; }
std::string batch_label(const RemoteInstallQueueEntry& entry) { return entry.entry.name; }
std::string batch_label(const std::string& path) { return basename_of(path); }

template <typename T>
bool edit_batch_queue(astranas::ui::Gui& gui, PadState& pad, astranas::ui::InputRouter& input,
                      bool& exit_requested, const std::string& title, std::vector<T>& items) {
    if (items.empty()) return false;
    std::size_t selected = 0;
    bool dirty = true;
    while (appletMainLoop()) {
        if (dirty) {
            std::vector<std::string> labels;
            labels.reserve(items.size());
            for (const auto& item : items) labels.push_back(batch_label(item));
            astranas::ui::draw_batch_queue(gui, title, labels, selected);
            dirty = false;
        }

        const auto frame = input.update(pad);
        if (frame.down & HidNpadButton_Plus) { exit_requested = true; return false; }
        if (frame.nav_once & HidNpadButton_Up) {
            selected = selected == 0 ? items.size() - 1 : selected - 1;
            dirty = true;
        }
        if (frame.nav_once & HidNpadButton_Down) {
            selected = (selected + 1) % items.size();
            dirty = true;
        }
        if ((frame.nav_once & HidNpadButton_Left) && selected > 0) {
            std::swap(items[selected], items[selected - 1]);
            --selected;
            dirty = true;
        }
        if ((frame.nav_once & HidNpadButton_Right) && selected + 1 < items.size()) {
            std::swap(items[selected], items[selected + 1]);
            ++selected;
            dirty = true;
        }

        if (frame.touch_tap && frame.touch_y >= 196 && frame.touch_y < 532) {
            const std::size_t rows = std::min<std::size_t>(7, items.size());
            const std::size_t begin = visible_begin(selected, items.size(), rows);
            const std::size_t row = static_cast<std::size_t>((frame.touch_y - 196) / 48);
            if (row < rows && begin + row < items.size()) {
                selected = begin + row;
                dirty = true;
            }
        }

        bool start_requested = (frame.down & HidNpadButton_A) != 0;
        bool back_requested = (frame.down & HidNpadButton_B) != 0;
        bool remove_requested = (frame.down & HidNpadButton_X) != 0;
        bool clear_requested = (frame.down & HidNpadButton_Y) != 0;
        if (frame.touch_tap) {
            switch (batch_queue_touch_action(frame.touch_x, frame.touch_y)) {
                case BatchQueueTouchAction::Start: start_requested = true; break;
                case BatchQueueTouchAction::Back: back_requested = true; break;
                case BatchQueueTouchAction::Remove: remove_requested = true; break;
                case BatchQueueTouchAction::Clear: clear_requested = true; break;
                case BatchQueueTouchAction::None: break;
            }
        }

        if (clear_requested) {
            items.clear();
            return false;
        }
        if (remove_requested) {
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(selected));
            if (items.empty()) return false;
            if (selected >= items.size()) selected = items.size() - 1;
            dirty = true;
        }
        if (start_requested) return true;
        if (back_requested) return false;
        svcSleepThread(10'000'000);
    }
    return false;
}

void select_remote_candidates(const std::vector<RemoteDirEntry>& entries,
                              std::vector<RemoteInstallQueueEntry>& queue, bool invert) {
    for (const auto& entry : entries) {
        if (entry.is_dir || detect_install_candidate(entry.path) == InstallCandidateKind::None) continue;
        const bool selected = queue_contains(queue, entry.path);
        if (invert) toggle_install_queue(queue, entry);
        else if (!selected) toggle_install_queue(queue, entry);
    }
}
} // namespace

void Runtime::handle_remote(const InputFrame& frame, ActionContext& ctx) {
    const std::size_t count = remote_entries_.size();
    if (frame.nav & HidNpadButton_Up) remote_sel_ = count ? (remote_sel_ == 0 ? count - 1 : remote_sel_ - 1) : 0;
    if (frame.nav & HidNpadButton_Down) remote_sel_ = count ? (remote_sel_ + 1) % count : 0;
    if (frame.touch_scroll_rows) clamp_scroll(remote_sel_, count, frame.touch_scroll_rows);

    bool touch_activate = false, touch_toggle = false, touch_back = false, touch_menu = false;
    if (frame.touch_tap && frame.touch_y >= 285 && frame.touch_y < 621) {
        const std::size_t begin = visible_begin(remote_sel_, count, 8);
        const std::size_t row = static_cast<std::size_t>((frame.touch_y - 285) / 42);
        if (begin + row < count) {
            remote_sel_ = begin + row;
            if (frame.touch_x >= 52 && frame.touch_x < 106) touch_toggle = true;
            else touch_activate = true;
        }
    }
    if (frame.touch_tap && frame.touch_y >= 640) {
        if (frame.touch_x < 260) touch_activate = true;
        else if (frame.touch_x < 490) touch_back = true;
        else if (frame.touch_x < 760) touch_toggle = true;
        else touch_menu = true;
    }

    auto delete_current = [&]() {
        if (remote_sel_ >= remote_entries_.size()) return;
        if (!remote_) { status_ = "NAS 尚未连接"; return; }
        const auto entry = remote_entries_[remote_sel_];
        const std::string bound = config_.protocol == "smb" && config_.share.empty()
            ? "/" : (config_.root.empty() ? "/" : config_.root);
        if (!remote_path_within(bound, entry.path) || entry.path == bound || entry.path == "/" || entry.identity == "smb-share") {
            status_ = "禁止删除 NAS 根目录或 SMB 共享根目录";
            return;
        }
        if (!remote_->supports_delete()) { status_ = remote_->protocol_name() + " 当前不支持删除"; return; }
        std::string detail = "将删除：" + entry.name + "\n位置：" + entry.path;
        if (entry.is_dir) detail += "\n文件夹内全部内容也会被递归删除。";
        detail += "\n此操作无法撤销。";
        if (!astranas::ui::confirm_action(gui_, pad_, exit_requested_, entry.is_dir ? "删除 NAS 文件夹" : "删除 NAS 文件",
                detail, "确认删除", "取消")) { if (!exit_requested_) status_ = "删除已取消"; return; }
        std::string error;
        if (!remote_->delete_entry(entry, true, error)) { status_ = friendly_error("NAS 删除失败", error); return; }
        install_queue_.erase(std::remove_if(install_queue_.begin(), install_queue_.end(),
            [&](const auto& queued) { return path_at_or_below(queued.entry.path, entry.path); }), install_queue_.end());
        refresh_remote();
        if (remote_sel_ >= remote_entries_.size() && remote_sel_ > 0) --remote_sel_;
        status_ = std::string("已删除 NAS ") + (entry.is_dir ? "文件夹：" : "文件：") + entry.name;
    };

    const u64 down = frame.down;
    if ((down & HidNpadButton_B) || touch_back) {
        const std::string bound = config_.protocol == "smb" && config_.share.empty() ? "/" : config_.root;
        const std::string child = remote_dir_;
        remote_dir_ = remote_parent_within(bound, remote_dir_);
        if (remote_dir_ != child) remote_restore_selection_path_ = child;
        refresh_remote();
        persist_remote_state();
        return;
    }

    if ((down & HidNpadButton_X) || touch_toggle) {
        if (remote_sel_ >= remote_entries_.size()) return;
        const auto& entry = remote_entries_[remote_sel_];
        if (entry.is_dir || detect_install_candidate(entry.path) == InstallCandidateKind::None) {
            status_ = "当前项目不是可安装文件；X 只用于勾选安装包";
            return;
        }
        toggle_install_queue(install_queue_, entry);
        status_ = queue_contains(install_queue_, entry.path)
            ? "已勾选 #" + std::to_string(install_queue_.size()) + "：" + entry.name
            : "已取消勾选：" + entry.name;
        return;
    }

    if ((down & HidNpadButton_Y) || touch_menu) {
        const int choice = astranas::ui::choose_action(
            gui_, pad_, input_, exit_requested_, "NAS 页面操作",
            "A 打开 · B 返回 · X 勾选。批量安装前可查看并调整 #1/#2/... 实际顺序。",
            {"批量安装 / 管理队列（" + std::to_string(install_queue_.size()) + "）",
             "选择工具", "刷新文件列表", "测速当前文件", "删除当前项目"});
        if (choice == 0) {
            if (install_queue_.empty()) { status_ = "安装队列为空；在安装包上按 X 勾选"; return; }
            if (!remote_) { status_ = "NAS 尚未连接"; return; }
            if (!prepare_remote_install_queue(*remote_, install_queue_, config_, ctx, status_)) return;
            const bool has_unavailable_order_info = std::any_of(
                install_queue_.begin(), install_queue_.end(), [](const auto& queued) {
                    return queued.info_state == RemoteInstallInfoState::Unavailable;
                });
            const std::string queue_title = has_unavailable_order_info
                ? "NAS 批量安装（保留当前顺序）" : "NAS 批量安装";
            if (edit_batch_queue(gui_, pad_, input_, exit_requested_, queue_title, install_queue_)) {
                run_install_queue(*remote_, install_queue_, config_, ctx, status_);
                cache_loaded_ = false;
            } else if (!exit_requested_ && install_queue_.empty()) {
                status_ = "安装队列已清空";
            }
        } else if (choice == 1) {
            const int select_choice = astranas::ui::choose_action(
                gui_, pad_, input_, exit_requested_, "NAS 选择工具",
                "只处理当前目录中的可安装文件；已选项目继续保留原来的先后顺序。",
                {"全选当前目录安装包", "反选当前目录安装包", "清空全部选择"});
            if (select_choice == 0) select_remote_candidates(remote_entries_, install_queue_, false);
            else if (select_choice == 1) select_remote_candidates(remote_entries_, install_queue_, true);
            else if (select_choice == 2) install_queue_.clear();
            if (select_choice >= 0) status_ = "当前安装队列：" + std::to_string(install_queue_.size()) + " 项";
        } else if (choice == 2) {
            refresh_remote();
            status_ = "NAS 文件列表已刷新";
        } else if (choice == 3) {
            if (!remote_ || remote_sel_ >= remote_entries_.size() || remote_entries_[remote_sel_].is_dir) {
                status_ = "请选择一个 NAS 文件后再测速";
            } else {
                double speed = 0.0;
                std::string error;
                if (remote_->benchmark(remote_entries_[remote_sel_].path, kBenchmarkBytes, speed, error)) {
                    last_benchmark_ = speed;
                    status_ = remote_->protocol_name() + " 测速：" + format_speed(speed);
                } else status_ = friendly_error("测速失败", error);
            }
        } else if (choice == 4) delete_current();
        return;
    }

    if (!(down & HidNpadButton_A) && !touch_activate) return;
    if (remote_sel_ >= remote_entries_.size()) return;
    const auto entry = remote_entries_[remote_sel_];
    if (entry.is_dir) {
        remote_dir_ = entry.path;
        remote_restore_selection_path_.clear();
        persist_remote_state();
        refresh_remote();
        remote_sel_ = 0;
        return;
    }
    if (!remote_) { status_ = "NAS 尚未连接"; return; }

    enum class RemoteFileAction { Download, Install, Benchmark, Delete };
    std::vector<std::string> labels;
    std::vector<RemoteFileAction> actions;
    labels.push_back("下载到本机当前目录"); actions.push_back(RemoteFileAction::Download);
    if (detect_install_candidate(entry.path) != InstallCandidateKind::None) {
        labels.push_back(config_.network_direct_install ? "安装（优先网络直装）" : "暂存并安装");
        actions.push_back(RemoteFileAction::Install);
    }
    labels.push_back("测速"); actions.push_back(RemoteFileAction::Benchmark);
    labels.push_back("删除"); actions.push_back(RemoteFileAction::Delete);

    const int choice = astranas::ui::choose_action(gui_, pad_, input_, exit_requested_, "NAS 文件操作",
        entry.name + "\n位置：" + entry.path + "\n\n提示：批量选择直接按 X。", labels);
    if (choice < 0 || static_cast<std::size_t>(choice) >= actions.size()) return;
    switch (actions[static_cast<std::size_t>(choice)]) {
        case RemoteFileAction::Download: {
            validate_local_state();
            AppConfig transfer_config = config_;
            transfer_config.local_root = local_root_;
            transfer_config.local_dir = local_dir_;
            const std::string destination = local_join_path(local_dir_, remote_cache_filename(transfer_config, entry));
            transfer_remote_file(*remote_, entry, transfer_config, destination, ctx, status_, "正在下载到本机当前目录");
            local_loaded_ = false;
            break;
        }
        case RemoteFileAction::Install:
            install_remote_entry(*remote_, entry, config_, ctx, status_);
            cache_loaded_ = false;
            break;
        case RemoteFileAction::Benchmark: {
            double speed = 0.0; std::string error;
            if (remote_->benchmark(entry.path, kBenchmarkBytes, speed, error)) {
                last_benchmark_ = speed;
                status_ = remote_->protocol_name() + " 测速：" + format_speed(speed);
            } else status_ = friendly_error("测速失败", error);
            break;
        }
        case RemoteFileAction::Delete: delete_current(); break;
    }
}

void Runtime::handle_local(const InputFrame& frame, ActionContext& ctx) {
    const std::size_t count = local_entries_.size();
    if (frame.nav & HidNpadButton_Up) local_sel_ = count ? (local_sel_ == 0 ? count - 1 : local_sel_ - 1) : 0;
    if (frame.nav & HidNpadButton_Down) local_sel_ = count ? (local_sel_ + 1) % count : 0;
    if (frame.touch_scroll_rows) clamp_scroll(local_sel_, count, frame.touch_scroll_rows);

    bool touch_activate = false, touch_toggle = false, touch_back = false, touch_menu = false;
    if (frame.touch_tap && frame.touch_y >= 285 && frame.touch_y < 621) {
        const std::size_t begin = visible_begin(local_sel_, count, 8);
        const std::size_t row = static_cast<std::size_t>((frame.touch_y - 285) / 42);
        if (begin + row < count) {
            local_sel_ = begin + row;
            if (frame.touch_x >= 52 && frame.touch_x < 106) touch_toggle = true;
            else touch_activate = true;
        }
    }
    if (frame.touch_tap && frame.touch_y >= 640) {
        if (frame.touch_x < 260) touch_activate = true;
        else if (frame.touch_x < 490) touch_back = true;
        else if (frame.touch_x < 760) touch_toggle = true;
        else touch_menu = true;
    }

    auto delete_current = [&]() {
        if (local_sel_ >= local_entries_.size()) return;
        const auto entry = local_entries_[local_sel_];
        std::string detail = "将删除：" + entry.name + "\n位置：" + entry.path;
        if (entry.is_dir) detail += "\n文件夹内全部内容也会被递归删除。";
        detail += "\n此操作无法撤销。";
        if (!astranas::ui::confirm_action(gui_, pad_, exit_requested_, entry.is_dir ? "删除本机文件夹" : "删除本机文件",
                detail, "确认删除", "取消")) { if (!exit_requested_) status_ = "删除已取消"; return; }
        std::string error;
        if (!delete_local_entry(local_root_, entry, error)) { status_ = friendly_error("删除失败", error); return; }
        std::remove((entry.path + ".astranas-meta").c_str());
        local_selection_.erase(std::remove_if(local_selection_.begin(), local_selection_.end(),
            [&](const auto& path) { return path_at_or_below(path, entry.path); }), local_selection_.end());
        if (move_pending_ && path_at_or_below(move_source_.path, entry.path)) {
            move_pending_ = false; move_source_ = {}; move_source_root_.clear();
        }
        refresh_local();
        if (local_sel_ >= local_entries_.size() && local_sel_ > 0) --local_sel_;
        status_ = std::string("已删除本机") + (entry.is_dir ? "文件夹：" : "文件：") + entry.name;
    };

    auto move_here = [&]() {
        if (!move_pending_) return;
        std::string destination, error;
        const std::string moved_name = move_source_.name;
        if (move_local_entry(move_source_root_, local_root_, move_source_, local_dir_, destination, error)) {
            local_selection_.erase(std::remove_if(local_selection_.begin(), local_selection_.end(),
                [&](const auto& path) { return path_at_or_below(path, move_source_.path); }), local_selection_.end());
            move_pending_ = false; move_source_ = {}; move_source_root_.clear();
            refresh_local();
            status_ = "已移动到当前目录：" + moved_name;
        } else status_ = friendly_error("移动失败", error);
    };

    const u64 down = frame.down;
    if (move_pending_ && ((down & HidNpadButton_B) || touch_back)) {
        move_pending_ = false; move_source_ = {}; move_source_root_.clear();
        status_ = "已取消移动";
        return;
    }
    if (!move_pending_ && ((down & HidNpadButton_B) || touch_back)) {
        local_dir_ = local_parent_within(local_root_, local_dir_);
        refresh_local();
        local_sel_ = 0;
        return;
    }

    if (!move_pending_ && ((down & HidNpadButton_X) || touch_toggle)) {
        if (local_sel_ >= local_entries_.size()) return;
        const auto& entry = local_entries_[local_sel_];
        if (detect_local_install_candidate(entry) == InstallCandidateKind::None) {
            status_ = "当前项目不是可安装文件；X 只用于勾选安装包";
            return;
        }
        toggle_local_selection(local_selection_, entry.path);
        const auto it = std::find(local_selection_.begin(), local_selection_.end(), entry.path);
        status_ = it != local_selection_.end()
            ? "已勾选 #" + std::to_string(static_cast<std::size_t>(std::distance(local_selection_.begin(), it)) + 1) + "：" + entry.name
            : "已取消勾选：" + entry.name;
        return;
    }

    if ((down & HidNpadButton_Y) || touch_menu) {
        if (move_pending_) {
            const int choice = astranas::ui::choose_action(gui_, pad_, input_, exit_requested_, "移动文件",
                "源项目：" + move_source_.name + "\n目标目录：" + local_dir_,
                {"移动到当前目录", "取消移动", "刷新当前目录"});
            if (choice == 0) move_here();
            else if (choice == 1) { move_pending_ = false; move_source_ = {}; move_source_root_.clear(); status_ = "已取消移动"; }
            else if (choice == 2) refresh_local();
            return;
        }
        const int choice = astranas::ui::choose_action(
            gui_, pad_, input_, exit_requested_, "本机页面操作",
            "A 打开 · B 返回 · X 勾选。批量安装前可查看并调整 #1/#2/... 实际顺序。",
            {"批量安装 / 管理选择（" + std::to_string(local_selection_.size()) + "）",
             "选择工具", "刷新文件列表", "切换 SD / USB", "更多操作"});
        if (choice == 0) {
            if (local_selection_.empty()) { status_ = "尚未选择安装包；在安装包上按 X 勾选"; return; }
            if (edit_batch_queue(gui_, pad_, input_, exit_requested_, "本机批量安装", local_selection_)) {
                install_local_batch(local_selection_, config_, ctx, status_, false);
                refresh_local();
            } else if (!exit_requested_ && local_selection_.empty()) status_ = "批量选择已清空";
        } else if (choice == 1) {
            const int select_choice = astranas::ui::choose_action(gui_, pad_, input_, exit_requested_, "本机选择工具",
                "只处理当前目录中的可安装文件；已选项目继续保留原来的先后顺序。",
                {"全选当前目录安装包", "反选当前目录安装包", "清空全部选择"});
            if (select_choice == 0) select_local_candidates(local_entries_, local_selection_, false);
            else if (select_choice == 1) select_local_candidates(local_entries_, local_selection_, true);
            else if (select_choice == 2) local_selection_.clear();
            if (select_choice >= 0) status_ = "当前批量选择：" + std::to_string(local_selection_.size()) + " 项";
        } else if (choice == 2) {
            refresh_local(); status_ = "本机文件列表已刷新；当前目录已记住";
        } else if (choice == 3) {
            ensure_usb();
            const auto locations = local_locations(usb_ready_);
            auto it = std::find_if(locations.begin(), locations.end(), [&](const auto& item) { return item.root == local_root_; });
            const std::size_t index = it == locations.end() ? 0 :
                (static_cast<std::size_t>(std::distance(locations.begin(), it)) + 1) % locations.size();
            local_root_ = locations[index].root;
            local_dir_ = local_root_;
            local_sel_ = 0;
            refresh_local();
            status_ = "已切换本机存储：" + locations[index].label;
        } else if (choice == 4) {
            const int more = astranas::ui::choose_action(gui_, pad_, input_, exit_requested_, "更多操作",
                "递归扫描会遍历当前目录及子目录；删除始终需要二次确认。",
                {"递归扫描并管理批量安装", "删除当前项目"});
            if (more == 0) {
                std::vector<std::string> candidates;
                std::string error;
                if (!collect_local_candidates(local_dir_, true, candidates, ctx, error)) {
                    status_ = friendly_error("递归扫描失败", error);
                } else if (candidates.empty()) {
                    status_ = "当前目录及子目录没有可安装文件";
                } else {
                    local_selection_ = std::move(candidates);
                    if (edit_batch_queue(gui_, pad_, input_, exit_requested_, "递归批量安装", local_selection_)) {
                        install_local_batch(local_selection_, config_, ctx, status_, true);
                        refresh_local();
                    }
                }
            } else if (more == 1) delete_current();
        }
        return;
    }

    if (!(down & HidNpadButton_A) && !touch_activate) return;
    if (local_sel_ >= local_entries_.size()) return;
    const auto entry = local_entries_[local_sel_];
    if (entry.is_dir) {
        local_dir_ = entry.path;
        refresh_local();
        local_sel_ = 0;
        return;
    }
    if (move_pending_) {
        status_ = "移动模式：A 打开文件夹；Y → 移动到当前目录；B 取消";
        return;
    }

    enum class LocalFileAction { Install, Upload, Move, Delete };
    std::vector<std::string> labels;
    std::vector<LocalFileAction> actions;
    if (detect_local_install_candidate(entry) != InstallCandidateKind::None) {
        labels.push_back("安装"); actions.push_back(LocalFileAction::Install);
    }
    labels.push_back("上传到 NAS 当前目录"); actions.push_back(LocalFileAction::Upload);
    labels.push_back("移动到其他目录"); actions.push_back(LocalFileAction::Move);
    labels.push_back("删除"); actions.push_back(LocalFileAction::Delete);

    const int choice = astranas::ui::choose_action(gui_, pad_, input_, exit_requested_, "本机文件操作",
        entry.name + "\n位置：" + entry.path + "\n\n提示：批量选择直接按 X。", labels);
    if (choice < 0 || static_cast<std::size_t>(choice) >= actions.size()) return;
    switch (actions[static_cast<std::size_t>(choice)]) {
        case LocalFileAction::Install:
            install_from_path(entry.path, config_, ctx, status_, config_.delete_source_after_install);
            refresh_local();
            break;
        case LocalFileAction::Upload: {
            if (!remote_) { remote_attempted_ = true; if (!connect_remote()) break; }
            AppConfig transfer_config = config_;
            transfer_config.local_root = local_root_;
            transfer_config.local_dir = local_dir_;
            if (upload_local_file(*remote_, entry, transfer_config, remote_dir_, ctx, status_)) {
                std::vector<RemoteDirEntry> refreshed;
                std::string refresh_error;
                if (remote_->list_dir(remote_dir_, refreshed, refresh_error)) remote_entries_ = std::move(refreshed);
                else append_debug_log("上传后刷新 NAS 目录", refresh_error);
            }
            break;
        }
        case LocalFileAction::Move:
            move_source_ = entry;
            move_source_root_ = local_root_;
            move_pending_ = true;
            status_ = "移动模式：浏览到目标目录后按 Y → 移动到当前目录；B 取消";
            break;
        case LocalFileAction::Delete: delete_current(); break;
    }
}

void Runtime::handle_cache(const InputFrame& frame, ActionContext& ctx) {
    const std::size_t count = cache_entries_.size();
    if (frame.nav & HidNpadButton_Up) cache_sel_ = count ? (cache_sel_ == 0 ? count - 1 : cache_sel_ - 1) : 0;
    if (frame.nav & HidNpadButton_Down) cache_sel_ = count ? (cache_sel_ + 1) % count : 0;
    if (frame.touch_scroll_rows) clamp_scroll(cache_sel_, count, frame.touch_scroll_rows);

    bool touch_activate = false, touch_menu = false;
    if (frame.touch_tap && frame.touch_y >= 285 && frame.touch_y < 621) {
        const std::size_t begin = visible_begin(cache_sel_, count, 8);
        const std::size_t row = static_cast<std::size_t>((frame.touch_y - 285) / 42);
        if (begin + row < count) { cache_sel_ = begin + row; touch_activate = true; }
    }
    if (frame.touch_tap && frame.touch_y >= 640) {
        if (frame.touch_x < 360) touch_activate = true;
        else touch_menu = true;
    }

    const u64 down = frame.down;
    if ((down & HidNpadButton_Y) || touch_menu) {
        const int choice = astranas::ui::choose_action(gui_, pad_, input_, exit_requested_, "缓存页面操作",
            "低频危险操作不再占用组合快捷键。", {"刷新缓存", "清空全部缓存"});
        if (choice == 0) {
            refresh_cache(); status_ = "安装缓存已刷新";
        } else if (choice == 1 && astranas::ui::confirm_action(gui_, pad_, exit_requested_, "清空全部安装缓存",
                "将删除 AstraNAS 安装缓存目录中的全部文件。\n此操作无法撤销。", "确认清空", "取消")) {
            std::string error;
            if (clear_local_directory(config_.cache_dir, error)) status_ = "安装缓存已清空";
            else status_ = friendly_error("清空缓存失败", error);
            refresh_cache(); cache_sel_ = 0;
        }
        return;
    }
    if (!((down & HidNpadButton_A) || touch_activate) || cache_sel_ >= cache_entries_.size()) return;

    const auto entry = cache_entries_[cache_sel_];
    enum class CacheFileAction { Install, Delete };
    std::vector<std::string> labels;
    std::vector<CacheFileAction> actions;
    if (detect_local_install_candidate(entry) != InstallCandidateKind::None) {
        labels.push_back("安装"); actions.push_back(CacheFileAction::Install);
    }
    labels.push_back("删除缓存文件"); actions.push_back(CacheFileAction::Delete);
    const int choice = astranas::ui::choose_action(gui_, pad_, input_, exit_requested_, "缓存文件操作",
        entry.name + "\n位置：" + entry.path, labels);
    if (choice < 0 || static_cast<std::size_t>(choice) >= actions.size()) return;
    switch (actions[static_cast<std::size_t>(choice)]) {
        case CacheFileAction::Install:
            install_from_cache(entry.path, config_, ctx, status_);
            refresh_cache();
            break;
        case CacheFileAction::Delete:
            if (astranas::ui::confirm_action(gui_, pad_, exit_requested_, "删除缓存项目",
                    "将删除：" + entry.name + "\n此操作无法撤销。", "确认删除", "取消")) {
                std::string error;
                if (delete_local_entry(config_.cache_dir, entry, error)) {
                    std::remove((entry.path + ".astranas-meta").c_str());
                    status_ = "已删除缓存：" + entry.name;
                } else status_ = friendly_error("删除缓存失败", error);
                refresh_cache();
            }
            break;
    }
}

} // namespace astranas::app
#endif
