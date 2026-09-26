// SPDX-License-Identifier: GPL-3.0-or-later
#include "actions.hpp"
#ifdef __SWITCH__
#include "util.hpp"
#include "../download_cache.hpp"
#include "../title_backend/package_source.hpp"
#include "../ui/dialogs.hpp"
#include <algorithm>
#include <utility>

namespace astranas::app {
namespace {
bool append_install_candidate(std::vector<std::string>& candidates, const std::string& path) {
    if (detect_install_candidate(path) == InstallCandidateKind::None) return true;
    if (std::find(candidates.begin(), candidates.end(), path) == candidates.end()) candidates.push_back(path);
    return candidates.size() <= 4096;
}
} // namespace

InstallCandidateKind detect_local_install_candidate(const LocalEntry& entry) {
    const auto kind = detect_install_candidate(entry.path);
    if (!entry.is_dir) return kind;
    if (kind != InstallCandidateKind::NintendoPackageNeedsBackend) return InstallCandidateKind::None;
    std::vector<std::string> parts;
    std::string ignored;
    return astranas::title_backend::resolve_package_parts(entry.path, parts, ignored) ? kind : InstallCandidateKind::None;
}
bool queue_contains(const std::vector<RemoteInstallQueueEntry>& queue, const std::string& path) {
    return std::any_of(queue.begin(), queue.end(), [&](const auto& item) { return item.entry.path == path; });
}
bool local_selection_contains(const std::vector<std::string>& selection, const std::string& path) {
    return std::find(selection.begin(), selection.end(), path) != selection.end();
}
void toggle_local_selection(std::vector<std::string>& selection, const std::string& path) {
    const auto it = std::find(selection.begin(), selection.end(), path);
    if (it == selection.end()) selection.push_back(path); else selection.erase(it);
}
void select_local_candidates(const std::vector<LocalEntry>& entries,
                             std::vector<std::string>& selection, bool invert) {
    for (const auto& entry : entries) {
        if (detect_local_install_candidate(entry) == InstallCandidateKind::None) continue;
        const bool selected = local_selection_contains(selection, entry.path);
        if (invert) toggle_local_selection(selection, entry.path);
        else if (!selected) selection.push_back(entry.path);
    }
}
void toggle_install_queue(std::vector<RemoteInstallQueueEntry>& queue, const RemoteDirEntry& entry) {
    const auto it = std::find_if(queue.begin(), queue.end(), [&](const auto& item) { return item.entry.path == entry.path; });
    if (it == queue.end()) {
        RemoteInstallQueueEntry queued{};
        queued.entry = entry;
        queue.push_back(std::move(queued));
    } else {
        queue.erase(it);
    }
}
bool collect_local_candidates(const std::string& directory, bool recursive,
                              std::vector<std::string>& candidates, ActionContext& ctx,
                              std::string& error, std::size_t depth) {
    if (depth > 32) { error = "递归扫描深度超过 32 层目录"; return false; }
    if (!appletMainLoop()) { error = "扫描已取消"; return false; }
    padUpdate(&ctx.pad);
    const u64 down = padGetButtonsDown(&ctx.pad);
    if (down & HidNpadButton_Plus) { ctx.exit_requested = true; error = "扫描已取消"; return false; }
    if (down & HidNpadButton_B) { error = "扫描已取消"; return false; }
    if (recursive) astranas::ui::draw_progress_page(ctx.gui, "正在扫描可安装文件", basename_of(directory),
                           "当前路径：" + directory + "  ·  已找到 " + std::to_string(candidates.size()) + " 个安装包", 0, 0, 0.0);
    std::vector<LocalEntry> entries;
    if (!list_local_dir(directory, entries, error)) return false;
    for (const auto& entry : entries) {
        if (!entry.is_dir && is_transfer_sidecar_name(entry.name)) continue;
        const auto kind = detect_local_install_candidate(entry);
        if (kind != InstallCandidateKind::None) {
            if (!append_install_candidate(candidates, entry.path)) { error = "递归扫描找到的安装包超过 4096 个，已停止"; return false; }
        } else if (recursive && entry.is_dir && !collect_local_candidates(entry.path, true, candidates, ctx, error, depth + 1)) return false;
    }
    return true;
}

} // namespace astranas::app
#endif
