// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifdef __SWITCH__
#include "../config.hpp"
#include "../installer.hpp"
#include "../local_fs.hpp"
#include "../model.hpp"
#include "../remote/remote_client.hpp"
#include "../ui/gui.hpp"
#include "remote_install_queue.hpp"
#include <string>
#include <vector>
#include <switch.h>

namespace astranas::app {

struct ActionContext {
    astranas::ui::Gui& gui;
    PadState& pad;
    bool& exit_requested;
};

InstallCandidateKind detect_local_install_candidate(const LocalEntry& entry);
bool queue_contains(const std::vector<RemoteInstallQueueEntry>& queue, const std::string& path);
bool local_selection_contains(const std::vector<std::string>& selection, const std::string& path);
void toggle_local_selection(std::vector<std::string>& selection, const std::string& path);
void select_local_candidates(const std::vector<LocalEntry>& entries,
                             std::vector<std::string>& selection, bool invert);
void toggle_install_queue(std::vector<RemoteInstallQueueEntry>& queue, const RemoteDirEntry& entry);

bool transfer_remote_file(RemoteClient& remote, const RemoteDirEntry& entry, const AppConfig& config,
                          const std::string& destination, ActionContext& ctx,
                          std::string& status, const char* phase);
bool upload_local_file(RemoteClient& remote, const LocalEntry& entry, const AppConfig& config,
                       const std::string& remote_directory, ActionContext& ctx,
                       std::string& status);
bool stage_remote_candidate(RemoteClient& remote, const RemoteDirEntry& entry,
                            const AppConfig& config, ActionContext& ctx,
                            std::string& status, std::string& staged_path);
bool collect_local_candidates(const std::string& directory, bool recursive,
                              std::vector<std::string>& candidates, ActionContext& ctx,
                              std::string& error, std::size_t depth = 0);
InstallResult install_from_path(const std::string& source_path, AppConfig& config,
                                ActionContext& ctx, std::string& status,
                                bool delete_after_success, bool ask_confirmation = true,
                                bool notify_on_success = true);
InstallResult install_from_cache(const std::string& cached_path, AppConfig& config,
                                 ActionContext& ctx, std::string& status,
                                 bool ask_confirmation = true,
                                 bool notify_on_success = true);
InstallResult install_remote_entry(RemoteClient& remote, const RemoteDirEntry& entry,
                                   AppConfig& config, ActionContext& ctx,
                                   std::string& status, bool ask_confirmation,
                                   bool notify_on_success = true);
// The NAS file action menu itself is confirmed with A. Wait until that button is
// released before opening the install preflight dialog, otherwise the same press can
// leak into the next screen and instantly choose the default skip action.
inline InstallResult install_remote_entry(RemoteClient& remote, const RemoteDirEntry& entry,
                                          AppConfig& config, ActionContext& ctx,
                                          std::string& status) {
    while (true) {
        if (!appletMainLoop()) { ctx.exit_requested = true; return InstallResult::Cancelled; }
        padUpdate(&ctx.pad);
        const u64 held = padGetButtons(&ctx.pad);
        if ((held & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_X)) == 0) break;
        svcSleepThread(10'000'000);
    }
    return install_remote_entry(remote, entry, config, ctx, status, true, true);
}
bool install_local_batch(const std::vector<std::string>& candidates, AppConfig& config,
                         ActionContext& ctx, std::string& status, bool recursive);
bool prepare_remote_install_queue(RemoteClient& remote,
                                  std::vector<RemoteInstallQueueEntry>& install_queue,
                                  AppConfig& config, ActionContext& ctx,
                                  std::string& status);
bool run_install_queue(RemoteClient& remote, std::vector<RemoteInstallQueueEntry>& install_queue,
                       AppConfig& config, ActionContext& ctx, std::string& status);

} // namespace astranas::app
#endif
